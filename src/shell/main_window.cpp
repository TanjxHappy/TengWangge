#include "shell/main_window.h"

#include "core/app_info.h"
#include "shell/log_panel.h"
#include "shell/status_strip.h"
#include "shell/theme.h"
#include "shell/tool_host.h"
#include "shell/tool_rail.h"
#include "shell/tool_view.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QVBoxLayout>
#include <QVariant>

namespace tw {
namespace {

QFrame *verticalSeparator(const QString &name, int height)
{
    auto *line = new QFrame;
    line->setObjectName(name);
    line->setFixedSize(1, height);
    return line;
}

QWidget *firstFocusable(QWidget *root)
{
    if (!root)
        return nullptr;
    QWidget *w = root;
    for (int i = 0; i < 500; ++i) {
        w = w->nextInFocusChain();
        if (!w || w == root)
            break;
        if (root->isAncestorOf(w) && w->isEnabled() && w->isVisibleTo(root->window())
            && (w->focusPolicy() & Qt::TabFocus))
            return w;
    }
    return (root->focusPolicy() & Qt::TabFocus) ? root : nullptr;
}

} // namespace

MainWindow::MainWindow(std::unique_ptr<PlatformServices> platform, ToolRegistry registry,
                       QWidget *parent)
    : QMainWindow(parent)
    , m_platform(std::move(platform))
    , m_registry(std::move(registry))
{
    Q_ASSERT(m_platform);

    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(AppInfo::productName());
    resize(1180, 760);
    setMinimumSize(900, 560);
    setStyleSheet(Theme::workbench().styleSheet());

    buildUi();

    m_host = new ToolHost(&m_registry, m_platform.get(), this, m_stack, this);
    connectServices();

    ToolServices self = m_platform->servicesFor(QString());
    self.logInfo(QStringLiteral("%1 %2 启动").arg(AppInfo::productName(), AppInfo::version()));
    self.logInfo(QStringLiteral("数据目录：%1").arg(m_platform->paths().root()));

    m_host->initializeAll();

    QVector<ToolInfo> infos;
    for (int i = 0; i < m_host->count(); ++i)
        infos.append(m_host->info(i));
    m_rail->setTools(infos);
    for (int i = 0; i < m_host->count(); ++i) {
        if (m_host->state(i) == ToolHost::State::Failed)
            m_rail->setAvailable(i, false, m_host->error(i));
    }
    self.logInfo(QStringLiteral("已加载 %1 个工具").arg(infos.size()));

    installShortcuts();

    if (m_host->count() == 0) {
        auto *empty = new QWidget;
        empty->setObjectName(QStringLiteral("emptyView"));
        empty->setAttribute(Qt::WA_StyledBackground, true);
        auto *layout = new QVBoxLayout(empty);
        auto *label = new QLabel(QStringLiteral("尚未安装任何工具"));
        label->setObjectName(QStringLiteral("emptyTitle"));
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);
        m_stack->addWidget(empty);
        refreshChrome();
        return;
    }

    int first = 0;
    for (int i = 0; i < m_host->count(); ++i) {
        if (m_host->state(i) == ToolHost::State::Ready) {
            first = i;
            break;
        }
    }
    selectTool(first);
}

MainWindow::~MainWindow()
{
    m_tornDown = true;
    m_platform->status()->disconnect(this);
    m_platform->gate()->disconnect(this);

    if (m_host)
        m_host->shutdown();
    delete takeCentralWidget();
    delete m_host;
    m_host = nullptr;
}

void MainWindow::buildUi()
{
    auto *central = new QWidget;
    central->setObjectName(QStringLiteral("centralArea"));
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 顶栏 ----
    auto *top = new QFrame;
    top->setObjectName(QStringLiteral("topBar"));
    top->setFixedHeight(48);
    auto *topLayout = new QHBoxLayout(top);
    topLayout->setContentsMargins(14, 0, 16, 0);
    topLayout->setSpacing(10);

    auto *mark = new QLabel(QStringLiteral("滕"));
    mark->setObjectName(QStringLiteral("brandMark"));
    mark->setFixedSize(28, 28);
    mark->setAlignment(Qt::AlignCenter);
    auto *name = new QLabel(AppInfo::productName());
    name->setObjectName(QStringLiteral("brandName"));
    auto *sub = new QLabel(QStringLiteral("工具箱平台"));
    sub->setObjectName(QStringLiteral("brandSub"));

    m_crumbTool = new QLabel;
    m_crumbTool->setObjectName(QStringLiteral("breadcrumbTool"));
    m_crumbSep = new QLabel(QStringLiteral("›"));
    m_crumbSep->setObjectName(QStringLiteral("breadcrumbSep"));
    m_crumbPage = new QLabel;
    m_crumbPage->setObjectName(QStringLiteral("breadcrumbPage"));
    auto *version = new QLabel(QStringLiteral("v%1").arg(AppInfo::version()));
    version->setObjectName(QStringLiteral("topVersion"));

    topLayout->addWidget(mark);
    topLayout->addWidget(name);
    topLayout->addWidget(sub);
    topLayout->addSpacing(6);
    topLayout->addWidget(verticalSeparator(QStringLiteral("topSeparator"), 22));
    topLayout->addSpacing(6);
    topLayout->addWidget(m_crumbTool);
    topLayout->addWidget(m_crumbSep);
    topLayout->addWidget(m_crumbPage);
    topLayout->addStretch(1);
    topLayout->addWidget(version);
    root->addWidget(top);

    // ---- 主体：工具栏 | (页面栈 / 日志) ----
    auto *body = new QWidget;
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    auto *railPanel = new QWidget;
    railPanel->setObjectName(QStringLiteral("railPanel"));
    railPanel->setAttribute(Qt::WA_StyledBackground, true);
    railPanel->setFixedWidth(196);
    auto *railLayout = new QVBoxLayout(railPanel);
    railLayout->setContentsMargins(0, 0, 0, 0);
    railLayout->setSpacing(0);
    auto *railHeader = new QLabel(QStringLiteral("工具"));
    railHeader->setObjectName(QStringLiteral("railHeader"));
    m_rail = new ToolRail;
    railLayout->addWidget(railHeader);
    railLayout->addWidget(m_rail, 1);
    bodyLayout->addWidget(railPanel);

    m_stack = new QStackedWidget;
    m_stack->setObjectName(QStringLiteral("toolStack"));
    m_log = new LogPanel(m_platform->log());
    m_log->hide();

    m_splitter = new QSplitter(Qt::Vertical);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(1);
    m_splitter->addWidget(m_stack);
    m_splitter->addWidget(m_log);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    bodyLayout->addWidget(m_splitter, 1);
    root->addWidget(body, 1);

    setCentralWidget(central);

    // ---- 状态栏 ----
    m_status = new StatusStrip;
    statusBar()->setSizeGripEnabled(false);
    statusBar()->setFixedHeight(28);
    statusBar()->addWidget(m_status, 1);
}

void MainWindow::connectServices()
{
    connect(m_rail, &QListWidget::currentRowChanged, this, &MainWindow::onRailRowChanged);
    connect(m_host, &ToolHost::viewCreated, this, &MainWindow::onViewCreated);
    connect(m_host, &ToolHost::toolFailed, this, [this](int index, const QString &error) {
        m_rail->setAvailable(index, false, error);
    });
    connect(m_platform->status(), &StatusBoard::changed, this, [this](const QString &toolId) {
        if (toolId == currentToolId())
            refreshStatus();
    });
    connect(m_platform->gate(), &NavigationGate::changed, this, &MainWindow::refreshNavigationLock);
    connect(m_status->logToggle(), &QPushButton::toggled, this,
            [this](bool on) { setLogPanelVisible(on); });
    connect(m_log, &LogPanel::closeRequested, this, [this] {
        setLogPanelVisible(false);
        if (m_rail->isEnabled())
            m_rail->setFocus(Qt::OtherFocusReason);
    });
    m_log->setTitleResolver([this](const QString &toolId) {
        const int index = m_registry.indexOf(toolId);
        return index >= 0 ? m_registry.infoAt(index).title : toolId;
    });
}

void MainWindow::installShortcuts()
{
    auto add = [this](const QKeySequence &keys, std::function<void()> action) {
        auto *shortcut = new QShortcut(keys, this);
        shortcut->setContext(Qt::WindowShortcut);
        connect(shortcut, &QShortcut::activated, this, std::move(action));
    };

    for (int i = 0; i < 9; ++i)
        add(QKeySequence(Qt::CTRL + Qt::Key_1 + i), [this, i] { selectTool(i); });
    add(QKeySequence(Qt::ALT + Qt::Key_Up), [this] { stepTool(-1); });
    add(QKeySequence(Qt::ALT + Qt::Key_Down), [this] { stepTool(+1); });
    add(QKeySequence(Qt::CTRL + Qt::Key_PageUp), [this] { stepPage(-1); });
    add(QKeySequence(Qt::CTRL + Qt::Key_PageDown), [this] { stepPage(+1); });
    add(QKeySequence(Qt::CTRL + Qt::Key_L), [this] { setLogPanelVisible(!isLogPanelVisible()); });
    add(QKeySequence(Qt::Key_F6), [this] { cycleFocus(); });
    add(QKeySequence(Qt::Key_F1), [this] { showShortcutHelp(); });
}

QString MainWindow::shortcutHelpText()
{
    return QStringLiteral(
        "Ctrl+1 … Ctrl+9\t切换到第 N 个工具\n"
        "Alt+↑ / Alt+↓\t上一个 / 下一个工具\n"
        "Ctrl+PageUp / PageDown\t当前工具的上一页 / 下一页\n"
        "F6\t在工具栏、页面条、页面、日志之间移动焦点\n"
        "Ctrl+L\t显示 / 隐藏日志面板\n"
        "↑ ↓（工具栏） ← →（页面条）\t逐项移动\n"
        "F1\t显示本帮助\n\n"
        "长时间任务进行期间导航被锁定，切换类快捷键暂时无效。");
}

void MainWindow::showShortcutHelp()
{
    QMessageBox::information(this, QStringLiteral("键盘操作"), shortcutHelpText());
}

// ---------------------------------------------------------------------------
// 导航
// ---------------------------------------------------------------------------

int MainWindow::toolCount() const
{
    return m_host ? m_host->count() : 0;
}

int MainWindow::currentToolIndex() const
{
    return m_host ? m_host->currentIndex() : -1;
}

QString MainWindow::currentToolId() const
{
    const int index = currentToolIndex();
    return index >= 0 ? m_host->info(index).id : QString();
}

ToolView *MainWindow::currentView() const
{
    return m_host ? m_host->view(currentToolIndex()) : nullptr;
}

bool MainWindow::navigationLocked() const
{
    return m_platform->gate()->isLocked();
}

bool MainWindow::selectTool(int index)
{
    if (!m_host || index < 0 || index >= m_host->count() || navigationLocked())
        return false;
    if (index != m_host->currentIndex())
        m_host->activate(index);
    syncRailSelection();
    refreshChrome();
    return true;
}

bool MainWindow::selectToolById(const QString &toolId)
{
    const int index = m_registry.indexOf(toolId);
    return index >= 0 && selectTool(index);
}

bool MainWindow::stepTool(int delta)
{
    const int target = currentToolIndex() + delta;
    if (delta == 0 || target < 0 || target >= toolCount())
        return false;
    return selectTool(target);
}

bool MainWindow::selectPage(int index)
{
    ToolView *view = currentView();
    if (!view || navigationLocked())
        return false;
    return view->setCurrentPage(index);
}

bool MainWindow::selectPageById(const QString &pageId)
{
    ToolView *view = currentView();
    if (!view)
        return false;
    return selectPage(view->indexOfPage(pageId));
}

bool MainWindow::stepPage(int delta)
{
    ToolView *view = currentView();
    if (!view || navigationLocked())
        return false;
    return view->stepPage(delta);
}

void MainWindow::onRailRowChanged(int row)
{
    if (m_syncingRail || row < 0)
        return;
    if (!selectTool(row))
        syncRailSelection();
}

void MainWindow::syncRailSelection()
{
    const int index = currentToolIndex();
    if (index < 0 || m_rail->currentRow() == index)
        return;
    m_syncingRail = true;
    m_rail->setCurrentRow(index);
    m_syncingRail = false;
}

void MainWindow::onViewCreated(int index)
{
    ToolView *view = m_host->view(index);
    if (!view)
        return;
    connect(view, &ToolView::currentPageChanged, this, [this, view] {
        if (view == currentView())
            refreshChrome();
    });
    view->setNavigationEnabled(!navigationLocked());
}

void MainWindow::refreshChrome()
{
    const int index = currentToolIndex();
    if (index < 0) {
        m_crumbTool->setText(QStringLiteral("无工具"));
        m_crumbSep->hide();
        m_crumbPage->hide();
        setWindowTitle(AppInfo::productName());
        refreshStatus();
        return;
    }

    const ToolInfo info = m_host->info(index);
    QString pageTitle;
    if (ToolView *view = currentView())
        pageTitle = view->pageInfo(view->currentPage()).title;

    m_crumbTool->setText(info.title);
    m_crumbPage->setText(pageTitle);
    m_crumbSep->setVisible(!pageTitle.isEmpty());
    m_crumbPage->setVisible(!pageTitle.isEmpty());

    setWindowTitle(pageTitle.isEmpty()
                       ? QStringLiteral("%1 — %2").arg(AppInfo::productName(), info.title)
                       : QStringLiteral("%1 — %2 · %3").arg(AppInfo::productName(), info.title, pageTitle));

    m_log->setCurrentTool(info.id, info.title);
    refreshStatus();
    refreshNavigationLock();
}

void MainWindow::refreshStatus()
{
    const int index = currentToolIndex();
    if (index >= 0 && m_host->state(index) == ToolHost::State::Failed) {
        m_status->setStatus({StatusState::Error,
                             QStringLiteral("“%1”不可用：%2")
                                 .arg(m_host->info(index).title, m_host->error(index))});
        return;
    }
    m_status->setStatus(m_platform->status()->current(currentToolId()));
}

void MainWindow::refreshNavigationLock()
{
    if (m_tornDown)
        return;
    NavigationGate *gate = m_platform->gate();
    const bool locked = gate->isLocked();

    if (locked && m_rail->isEnabled()) {
        QWidget *focused = QApplication::focusWidget();
        const bool inNavigation = focused
            && (focused == m_rail
                || (currentView() && focused == currentView()->tabBar()));
        m_focusBeforeLock = inNavigation ? focused : nullptr;
    }

    m_rail->setEnabled(!locked);
    for (int i = 0; i < toolCount(); ++i) {
        if (ToolView *view = m_host->view(i))
            view->setNavigationEnabled(!locked);
    }
    m_status->setLocked(locked, locked ? gate->reason() : QString());

    if (!locked && m_focusBeforeLock) {
        m_focusBeforeLock->setFocus(Qt::OtherFocusReason);
        m_focusBeforeLock.clear();
    }
}

// ---------------------------------------------------------------------------
// 焦点与日志面板
// ---------------------------------------------------------------------------

void MainWindow::cycleFocus()
{
    QVector<QWidget *> zones;
    auto addZone = [this, &zones](QWidget *w) {
        if (w && w->isEnabled() && w->isVisibleTo(this))
            zones.append(w);
    };

    addZone(m_rail);
    if (ToolView *view = currentView()) {
        addZone(view->tabBar());
        addZone(firstFocusable(view->pageWidget(view->currentPage())));
    } else {
        addZone(firstFocusable(m_stack->currentWidget()));
    }
    if (m_log->isVisibleTo(this))
        addZone(m_log->view());
    addZone(m_status->logToggle());

    if (zones.isEmpty())
        return;

    ToolView *view = currentView();
    QWidget *pageRoot = view ? view->pageWidget(view->currentPage()) : m_stack->currentWidget();
    QWidget *focused = QApplication::focusWidget();
    int current = -1;
    for (int i = 0; i < zones.size() && focused; ++i) {
        QWidget *zone = zones.at(i);
        const bool isPageZone = pageRoot && pageRoot->isAncestorOf(zone);
        const bool inside = focused == zone
            || (isPageZone ? pageRoot->isAncestorOf(focused) || focused == pageRoot
                           : zone->isAncestorOf(focused));
        if (inside) {
            current = i;
            break;
        }
    }
    zones.at((current + 1) % zones.size())->setFocus(Qt::TabFocusReason);
}

void MainWindow::setLogPanelVisible(bool visible)
{
    if (isLogPanelVisible() != visible) {
        if (visible) {
            m_log->show();
            const int total = m_splitter->height();
            if (total > 0)
                m_splitter->setSizes({qMax(120, total - 220), 220});
        } else {
            m_log->hide();
        }
    }
    QPushButton *toggle = m_status->logToggle();
    if (toggle->isChecked() != visible) {
        const bool blocked = toggle->blockSignals(true);
        toggle->setChecked(visible);
        toggle->blockSignals(blocked);
    }
}

bool MainWindow::isLogPanelVisible() const
{
    return !m_log->isHidden();
}

void MainWindow::setCloseGuard(std::function<bool(const QString &)> guard)
{
    m_closeGuard = std::move(guard);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (navigationLocked()) {
        const QString reason = m_platform->gate()->reason();
        bool allow = false;
        if (m_closeGuard) {
            allow = m_closeGuard(reason);
        } else {
            const auto answer = QMessageBox::question(
                this, QStringLiteral("任务仍在进行"),
                QStringLiteral("当前有任务正在进行（%1）。\n现在退出可能使任务中断，确定退出吗？").arg(reason),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            allow = answer == QMessageBox::Yes;
        }
        if (!allow) {
            event->ignore();
            return;
        }
    }
    QMainWindow::closeEvent(event);
}

} // namespace tw
