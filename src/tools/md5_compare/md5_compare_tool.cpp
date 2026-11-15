#include "tools/md5_compare/md5_compare_tool.h"

#include "shell/elided_label.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractTableModel>
#include <QComboBox>
#include <QCoreApplication>
#include <QFont>
#include <QFontMetrics>
#include <QSizePolicy>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSettings>
#include <QTableView>
#include <QThread>
#include <QVBoxLayout>
#include <QVariant>

namespace tw {
namespace {

QString emptyCounts()
{
    return QStringLiteral("相同 0 · 内容不同 0 · 仅在目录 A 0 · 仅在目录 B 0 · 问题 0");
}

QString formatCounts(const CompareResult &result)
{
    return QStringLiteral("相同 %1 · 内容不同 %2 · 仅在目录 A %3 · 仅在目录 B %4 · 问题 %5")
        .arg(result.sameCount)
        .arg(result.differentCount)
        .arg(result.onlyInACount)
        .arg(result.onlyInBCount)
        .arg(result.problemCount);
}

QString formatProblems(const CompareResult &result)
{
    QStringList lines;
    for (const CompareProblem &problem : result.problems) {
        const QString where = problem.relativePath.isEmpty() ? QStringLiteral("（整体）")
                                                              : problem.relativePath;
        const QString who = problem.side == QLatin1String("A")   ? QStringLiteral("目录 A")
                            : problem.side == QLatin1String("B") ? QStringLiteral("目录 B")
                                                                 : problem.side;
        lines.append(QStringLiteral("[%1] %2：%3").arg(who, where, problem.message));
    }
    return lines.join(QLatin1Char('\n'));
}

// 普通 QScrollArea 在 widgetResizable 下不会小于子控件的 sizeHint。
// 表格和问题框的 sizeHint 很高，窗口里就会只剩滚动条、把下半截推出画面。
// 这里按最小高度撑开：放得下就铺满视口，放不下再滚动，输入框不会被压成 0。
class FittingScroll : public QScrollArea
{
public:
    explicit FittingScroll(QWidget *parent = nullptr)
        : QScrollArea(parent)
    {
        setWidgetResizable(false);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setAutoFillBackground(false);
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QScrollArea::resizeEvent(event);
        fit();
    }

    void showEvent(QShowEvent *event) override
    {
        QScrollArea::showEvent(event);
        fit();
    }

private:
    void fit()
    {
        QWidget *child = widget();
        if (!child || viewport() == nullptr)
            return;
        const QSize minimum = child->minimumSizeHint();
        const QSize target(qMax(minimum.width(), viewport()->width()),
                           qMax(minimum.height(), viewport()->height()));
        if (child->size() != target)
            child->resize(target);
    }
};

QLabel *makeLabel(const QString &text, const char *role, bool wrap = true)
{
    auto *label = new QLabel(text);
    label->setProperty("role", QString::fromLatin1(role));
    label->setWordWrap(wrap);
    if (!wrap)
        label->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    return label;
}

} // namespace

class CompareResultModel : public QAbstractTableModel
{
public:
    explicit CompareResultModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
        m_mono.setStyleHint(QFont::Monospace);
        m_mono.setFamily(QStringLiteral("monospace"));
    }

    void setEntries(const QVector<CompareEntry> &entries)
    {
        beginResetModel();
        m_entries = entries;
        rebuild();
        endResetModel();
    }

    void setFilter(int statusFilter, const QString &query)
    {
        if (m_filter == statusFilter && m_query == query)
            return;
        beginResetModel();
        m_filter = statusFilter;
        m_query = query;
        rebuild();
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return m_visible.size();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return 5;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
            return {};
        const CompareEntry &entry = m_entries.at(m_visible.at(index.row()));
        if (role == Qt::UserRole) {
            if (index.column() == 0)
                return entry.relativePath;
            if (index.column() == 1)
                return static_cast<int>(entry.status);
            return {};
        }
        if (role == Qt::FontRole && (index.column() == 2 || index.column() == 3))
            return m_mono;
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
            return {};

        QString text;
        switch (index.column()) {
        case 0:
            text = entry.relativePath.isEmpty() ? QStringLiteral("（整体）") : entry.relativePath;
            break;
        case 1:
            text = compareStatusLabel(entry.status);
            break;
        case 2:
            text = entry.md5A.isEmpty() ? QStringLiteral("—") : entry.md5A;
            break;
        case 3:
            text = entry.md5B.isEmpty() ? QStringLiteral("—") : entry.md5B;
            break;
        case 4:
            text = entry.detail;
            break;
        default:
            break;
        }
        return text;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        switch (section) {
        case 0:
            return QStringLiteral("相对路径");
        case 1:
            return QStringLiteral("状态");
        case 2:
            return QStringLiteral("目录 A 的 MD5");
        case 3:
            return QStringLiteral("目录 B 的 MD5");
        case 4:
            return QStringLiteral("说明");
        default:
            return {};
        }
    }

private:
    void rebuild()
    {
        m_visible.clear();
        for (int i = 0; i < m_entries.size(); ++i) {
            if (compareEntryMatches(m_entries.at(i), m_filter, m_query))
                m_visible.append(i);
        }
    }

    QVector<CompareEntry> m_entries;
    QVector<int> m_visible;
    int m_filter = -1;
    QString m_query;
    QFont m_mono;
};

class CompareThread : public QThread
{
    Q_OBJECT

public:
    CompareThread(QString dirA, QString dirB, std::atomic<bool> *cancel)
        : m_dirA(std::move(dirA))
        , m_dirB(std::move(dirB))
        , m_cancelRequested(cancel)
    {
    }

    CompareResult result() const { return m_result; }
    QString error() const { return m_error; }

signals:
    void progressUpdated(bool enumerating, int completed, int total, const QString &path);

protected:
    void run() override
    {
        QElapsedTimer timer;
        timer.start();
        qint64 lastEmit = -1000;
        const auto progress = [this, &timer, &lastEmit](const CompareProgress &update) {
            const bool last = !update.enumerating && update.total > 0 && update.completed >= update.total;
            const qint64 now = timer.elapsed();
            if (!last && now - lastEmit < 80)
                return;
            lastEmit = now;
            emit progressUpdated(update.enumerating, update.completed, update.total, update.currentPath);
        };
        const auto shouldCancel = [this]() {
            return m_cancelRequested != nullptr && m_cancelRequested->load(std::memory_order_acquire);
        };
        m_error.clear();
        m_result = {};
        if (!compareDirectories(m_dirA, m_dirB, &m_result, &m_error, progress, shouldCancel) && m_error.isEmpty()
            && !m_result.cancelled)
            m_error = QStringLiteral("比较失败");
    }

private:
    QString m_dirA;
    QString m_dirB;
    std::atomic<bool> *m_cancelRequested = nullptr;
    CompareResult m_result;
    QString m_error;
};

class ExportThread : public QThread
{
public:
    ExportThread(QString path, CompareResult result, std::atomic<bool> *cancel)
        : m_path(std::move(path))
        , m_result(std::move(result))
        , m_cancelRequested(cancel)
    {
    }

    bool ok() const { return m_ok; }
    QString error() const { return m_error; }

protected:
    void run() override
    {
        const auto shouldCancel = [this]() {
            return m_cancelRequested != nullptr && m_cancelRequested->load(std::memory_order_acquire);
        };
        m_ok = writeCompareReport(m_path, m_result, &m_error, shouldCancel);
    }

private:
    QString m_path;
    CompareResult m_result;
    std::atomic<bool> *m_cancelRequested = nullptr;
    bool m_ok = false;
    QString m_error;
};

Md5ComparePage::Md5ComparePage(const ToolServices &services, QWidget *parent)
    : QWidget(parent)
    , m_services(services)
{
    setAccessibleName(QStringLiteral("目录 MD5 比较"));

    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);

    layout->addWidget(makeLabel(QStringLiteral("目录 MD5 比较"), "pageTitle"));
    layout->addWidget(makeLabel(
        QStringLiteral("选择两个已存在的目录，递归比较其中的普通文件。按相对路径对齐并计算 MD5。"
                       "指向起始目录之外或会形成循环的符号链接会记入问题，不会跟随。"),
        "pageSubtitle"));

    auto *card = new QFrame;
    card->setProperty("role", QStringLiteral("card"));
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto *grid = new QGridLayout(card);
    grid->setContentsMargins(16, 14, 16, 14);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);
    grid->setColumnStretch(1, 1);

    auto *labelA = makeLabel(QStringLiteral("目录 A"), "fieldLabel", false);
    auto *labelB = makeLabel(QStringLiteral("目录 B"), "fieldLabel", false);
    m_dirA = new QLineEdit;
    m_dirB = new QLineEdit;
    m_dirA->setObjectName(QStringLiteral("md5DirA"));
    m_dirB->setObjectName(QStringLiteral("md5DirB"));
    m_dirA->setPlaceholderText(QStringLiteral("选择或输入目录 A"));
    m_dirB->setPlaceholderText(QStringLiteral("选择或输入目录 B"));
    m_dirA->setAccessibleName(QStringLiteral("目录 A"));
    m_dirB->setAccessibleName(QStringLiteral("目录 B"));
    m_dirA->setClearButtonEnabled(true);
    m_dirB->setClearButtonEnabled(true);
    m_dirA->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_dirB->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_dirA->setMinimumWidth(0);
    m_dirB->setMinimumWidth(0);
    const int editHeight = qMax(1, m_dirA->sizeHint().height());
    m_dirA->setMinimumHeight(editHeight);
    m_dirB->setMinimumHeight(editHeight);
    labelA->setBuddy(m_dirA);
    labelB->setBuddy(m_dirB);

    m_browseA = new QPushButton(QStringLiteral("浏览…"));
    m_browseB = new QPushButton(QStringLiteral("浏览…"));
    m_browseA->setObjectName(QStringLiteral("md5BrowseA"));
    m_browseB->setObjectName(QStringLiteral("md5BrowseB"));
    m_browseA->setAccessibleName(QStringLiteral("选择目录 A"));
    m_browseB->setAccessibleName(QStringLiteral("选择目录 B"));
    m_browseA->setAutoDefault(false);
    m_browseB->setAutoDefault(false);
    m_browseA->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_browseB->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);

    grid->addWidget(labelA, 0, 0);
    grid->addWidget(m_dirA, 0, 1);
    grid->addWidget(m_browseA, 0, 2);
    grid->addWidget(labelB, 1, 0);
    grid->addWidget(m_dirB, 1, 1);
    grid->addWidget(m_browseB, 1, 2);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    m_start = new QPushButton(QStringLiteral("开始比较"));
    m_cancel = new QPushButton(QStringLiteral("取消"));
    m_export = new QPushButton(QStringLiteral("导出报告"));
    m_start->setObjectName(QStringLiteral("md5Start"));
    m_cancel->setObjectName(QStringLiteral("md5Cancel"));
    m_export->setObjectName(QStringLiteral("md5Export"));
    m_start->setProperty("role", QStringLiteral("primary"));
    m_cancel->setProperty("role", QStringLiteral("danger"));
    m_start->setAccessibleName(QStringLiteral("开始比较"));
    m_cancel->setAccessibleName(QStringLiteral("取消比较"));
    m_export->setAccessibleName(QStringLiteral("导出报告"));
    m_start->setAutoDefault(false);
    m_cancel->setAutoDefault(false);
    m_export->setAutoDefault(false);
    m_start->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_cancel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_export->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    buttons->addWidget(m_start);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_export);
    buttons->addStretch(1);
    grid->addLayout(buttons, 2, 0, 1, 3);

    m_bar = new QProgressBar;
    m_bar->setObjectName(QStringLiteral("md5Progress"));
    m_bar->setAccessibleName(QStringLiteral("比较进度"));
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    m_bar->setTextVisible(true);
    m_bar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    grid->addWidget(m_bar, 3, 0, 1, 3);

    m_state = new ElidedLabel;
    m_state->setProperty("role", QStringLiteral("sectionTitle"));
    m_state->setObjectName(QStringLiteral("md5State"));
    m_state->setFullText(QStringLiteral("尚未比较"));
    grid->addWidget(m_state, 4, 0, 1, 3);

    grid->addWidget(makeLabel(QStringLiteral("不会修改这两个目录。上次使用的路径只写在本工具自己的数据里。"),
                              "hint"),
                    5, 0, 1, 3);
    layout->addWidget(card);

    m_counts = makeLabel(emptyCounts(), "sectionTitle");
    m_counts->setObjectName(QStringLiteral("md5Counts"));
    layout->addWidget(m_counts);

    auto *filters = new QHBoxLayout;
    filters->setSpacing(8);
    auto *filterLabel = makeLabel(QStringLiteral("状态"), "fieldLabel", false);
    auto *searchLabel = makeLabel(QStringLiteral("路径"), "fieldLabel", false);
    m_filter = new QComboBox;
    m_search = new QLineEdit;
    m_filter->setObjectName(QStringLiteral("md5Filter"));
    m_search->setObjectName(QStringLiteral("md5Search"));
    m_filter->setAccessibleName(QStringLiteral("按状态过滤"));
    m_search->setAccessibleName(QStringLiteral("按路径搜索"));
    m_search->setPlaceholderText(QStringLiteral("输入相对路径的一部分"));
    m_search->setClearButtonEnabled(true);
    m_search->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_search->setMinimumWidth(0);
    m_filter->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_filter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_filter->setMinimumContentsLength(6);
    filterLabel->setBuddy(m_filter);
    searchLabel->setBuddy(m_search);
    m_filter->addItem(QStringLiteral("全部"), -1);
    m_filter->addItem(compareStatusLabel(CompareStatus::Same), static_cast<int>(CompareStatus::Same));
    m_filter->addItem(compareStatusLabel(CompareStatus::Different),
                      static_cast<int>(CompareStatus::Different));
    m_filter->addItem(compareStatusLabel(CompareStatus::OnlyInA), static_cast<int>(CompareStatus::OnlyInA));
    m_filter->addItem(compareStatusLabel(CompareStatus::OnlyInB), static_cast<int>(CompareStatus::OnlyInB));
    m_filter->addItem(compareStatusLabel(CompareStatus::Problem), static_cast<int>(CompareStatus::Problem));
    filters->addWidget(filterLabel);
    filters->addWidget(m_filter);
    filters->addWidget(searchLabel);
    filters->addWidget(m_search, 1);
    layout->addLayout(filters);

    m_visible = makeLabel(QStringLiteral("当前显示 0 条"), "hint");
    m_visible->setObjectName(QStringLiteral("md5Visible"));
    layout->addWidget(m_visible);

    m_model = new CompareResultModel(this);
    m_table = new QTableView;
    m_table->setObjectName(QStringLiteral("md5Table"));
    m_table->setAccessibleName(QStringLiteral("比较结果"));
    m_table->setModel(m_model);
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideMiddle);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_table->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    const int textHeight = qMax(1, m_table->fontMetrics().height());
    const int headerHeight = qMax(textHeight, m_table->horizontalHeader()->sizeHint().height());
    m_table->setMinimumHeight(headerHeight + textHeight * 2);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    const int columnFloor = qMax(1, m_table->fontMetrics().horizontalAdvance(QStringLiteral("MD5")));
    m_table->horizontalHeader()->setMinimumSectionSize(columnFloor);
    const int statusWidth = m_table->fontMetrics().horizontalAdvance(QStringLiteral("仅在目录 B")) + 28;
    m_table->setColumnWidth(1, statusWidth);
    layout->addWidget(m_table, 3);

    layout->addWidget(makeLabel(QStringLiteral("问题"), "sectionTitle"));
    m_problems = new QPlainTextEdit;
    m_problems->setObjectName(QStringLiteral("md5Problems"));
    m_problems->setAccessibleName(QStringLiteral("问题列表"));
    m_problems->setReadOnly(true);
    m_problems->setPlaceholderText(QStringLiteral("没有问题"));
    m_problems->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_problems->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_problems->setMinimumHeight(m_problems->fontMetrics().height() * 2 + 8);
    layout->addWidget(m_problems, 1);

    auto *scroll = new FittingScroll;
    scroll->setObjectName(QStringLiteral("md5Scroll"));
    scroll->setWidget(body);
    scroll->viewport()->setAutoFillBackground(false);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    connect(m_browseA, &QPushButton::clicked, this, [this]() {
        browse(m_dirA, QStringLiteral("选择目录 A"));
    });
    connect(m_browseB, &QPushButton::clicked, this, [this]() {
        browse(m_dirB, QStringLiteral("选择目录 B"));
    });
    connect(m_start, &QPushButton::clicked, this, &Md5ComparePage::startCompare);
    connect(m_cancel, &QPushButton::clicked, this, &Md5ComparePage::cancelCompare);
    connect(m_export, &QPushButton::clicked, this, &Md5ComparePage::exportReport);
    connect(m_dirA, &QLineEdit::editingFinished, this, &Md5ComparePage::saveState);
    connect(m_dirB, &QLineEdit::editingFinished, this, &Md5ComparePage::saveState);
    connect(m_dirA, &QLineEdit::returnPressed, this, &Md5ComparePage::startCompare);
    connect(m_dirB, &QLineEdit::returnPressed, this, &Md5ComparePage::startCompare);
    connect(m_filter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { applyFilter(); });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &) { applyFilter(); });

    setTabOrder(m_dirA, m_browseA);
    setTabOrder(m_browseA, m_dirB);
    setTabOrder(m_dirB, m_browseB);
    setTabOrder(m_browseB, m_start);
    setTabOrder(m_start, m_cancel);
    setTabOrder(m_cancel, m_export);
    setTabOrder(m_export, m_filter);
    setTabOrder(m_filter, m_search);
    setTabOrder(m_search, m_table);
    setTabOrder(m_table, m_problems);

    loadState();
    applyFilter();
    updateActions();
}

Md5ComparePage::~Md5ComparePage()
{
    abort();
}

void Md5ComparePage::abort()
{
    m_cancelRequested.store(true, std::memory_order_release);
    CompareThread *compare = m_thread;
    ExportThread *exporter = m_exportThread;
    m_thread = nullptr;
    m_exportThread = nullptr;
    if (compare)
        disconnect(compare, nullptr, this, nullptr);
    if (exporter)
        disconnect(exporter, nullptr, this, nullptr);
    if (compare || exporter)
        QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    if (compare) {
        compare->wait();
        delete compare;
    }
    if (exporter) {
        exporter->wait();
        delete exporter;
    }
    m_running = false;
    m_lock.release();
}

void Md5ComparePage::browse(QLineEdit *edit, const QString &title)
{
    if (m_running || edit == nullptr)
        return;
    const QString current = edit->text().trimmed();
    const QString start = QFileInfo(current).isDir() ? current : QDir::homePath();
    const QString picked = QFileDialog::getExistingDirectory(this, title, start);
    if (picked.isEmpty())
        return;
    edit->setText(QDir::cleanPath(picked));
    saveState();
}

void Md5ComparePage::startCompare()
{
    if (m_running)
        return;
    const QString dirA = m_dirA->text().trimmed();
    const QString dirB = m_dirB->text().trimmed();
    m_dirA->setText(dirA);
    m_dirB->setText(dirB);

    const auto invalid = [](const QString &path, const QString &label) -> QString {
        if (path.isEmpty())
            return QStringLiteral("请选择%1").arg(label);
        const QFileInfo info(path);
        if (!info.exists())
            return QStringLiteral("%1不存在：%2").arg(label, path);
        if (!info.isDir())
            return QStringLiteral("%1不是目录：%2").arg(label, path);
        return {};
    };
    const QString errorA = invalid(dirA, QStringLiteral("目录 A"));
    if (!errorA.isEmpty()) {
        showFailure(errorA);
        return;
    }
    const QString errorB = invalid(dirB, QStringLiteral("目录 B"));
    if (!errorB.isEmpty()) {
        showFailure(errorB);
        return;
    }

    saveState();
    m_activeA = dirA;
    m_activeB = dirB;
    m_hasResult = false;
    m_comparedA.clear();
    m_comparedB.clear();
    m_result = {};
    m_model->setEntries({});
    m_problems->clear();
    m_counts->setText(emptyCounts());
    applyFilter();
    m_bar->setRange(0, 0);
    m_state->setFullText(QStringLiteral("正在列出文件…"));
    m_services.logInfo(QStringLiteral("开始比较：%1 与 %2").arg(dirA, dirB));
    m_services.showStatus(StatusState::Busy, QStringLiteral("正在比较目录"));

    m_cancelRequested.store(false, std::memory_order_release);
    m_thread = new CompareThread(dirA, dirB, &m_cancelRequested);
    m_thread->setObjectName(QStringLiteral("md5-compare"));
    connect(m_thread, &CompareThread::progressUpdated, this, &Md5ComparePage::onProgress,
            Qt::QueuedConnection);
    connect(m_thread, &QThread::finished, this, &Md5ComparePage::onFinished, Qt::QueuedConnection);
    m_running = true;
    m_lock = m_services.lockNavigation(QStringLiteral("正在比较目录 MD5"));
    updateActions();
    m_thread->start();
}

void Md5ComparePage::cancelCompare()
{
    if (!m_running)
        return;
    m_cancelRequested.store(true, std::memory_order_release);
    m_state->setFullText(QStringLiteral("正在取消…"));
    m_services.showStatus(StatusState::Busy, QStringLiteral("正在取消比较"));
}

void Md5ComparePage::exportReport()
{
    if (!m_hasResult || m_running)
        return;
    QString dirError;
    QString startDir = m_services.dataDir(&dirError);
    if (startDir.isEmpty())
        startDir = QDir::homePath();

    QFileDialog dialog(this, QStringLiteral("导出比较报告"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilter(QStringLiteral("CSV 文件 (*.csv)"));
    dialog.setDefaultSuffix(QStringLiteral("csv"));
    dialog.setDirectory(startDir);
    dialog.selectFile(QStringLiteral("md5-compare.csv"));
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString path = dialog.selectedFiles().value(0);
    if (path.isEmpty())
        return;
    exportTo(path);
}

void Md5ComparePage::exportTo(const QString &path)
{
    if (!m_hasResult || m_running)
        return;
    const QString dirA = m_comparedA.isEmpty() ? m_dirA->text() : m_comparedA;
    const QString dirB = m_comparedB.isEmpty() ? m_dirB->text() : m_comparedB;
    const QString blocked = compareReportBlockReason(dirA, dirB, path);
    if (!blocked.isEmpty()) {
        showFailure(blocked);
        return;
    }

    m_cancelRequested.store(false, std::memory_order_release);
    m_exportThread = new ExportThread(path, m_result, &m_cancelRequested);
    m_exportThread->setObjectName(QStringLiteral("md5-export"));
    connect(m_exportThread, &QThread::finished, this, &Md5ComparePage::onExportFinished, Qt::QueuedConnection);
    m_running = true;
    m_bar->setRange(0, 0);
    m_state->setFullText(QStringLiteral("正在导出报告…"));
    m_services.showStatus(StatusState::Busy, QStringLiteral("正在导出报告"));
    m_lock = m_services.lockNavigation(QStringLiteral("正在导出比较报告"));
    updateActions();
    m_exportThread->start();
}

void Md5ComparePage::onProgress(bool enumerating, int completed, int total, const QString &path)
{
    if (!m_running)
        return;
    if (enumerating || total <= 0) {
        m_bar->setRange(0, 0);
        const QString where = path.isEmpty() ? QString() : QStringLiteral("：%1").arg(path);
        m_state->setFullText(QStringLiteral("正在列出文件，已找到 %1 个%2").arg(completed).arg(where));
        m_services.showStatus(StatusState::Busy, QStringLiteral("正在列出文件"));
        return;
    }
    m_bar->setRange(0, total);
    m_bar->setValue(qBound(0, completed, total));
    m_state->setFullText(QStringLiteral("正在计算 MD5，第 %1/%2 个：%3").arg(completed).arg(total).arg(path));
    m_services.showStatus(StatusState::Busy, QStringLiteral("正在计算 MD5 %1/%2").arg(completed).arg(total));
}

void Md5ComparePage::onFinished()
{
    if (!m_thread)
        return;
    const CompareResult result = m_thread->result();
    const QString error = m_thread->error();
    CompareThread *thread = m_thread;
    m_thread = nullptr;
    thread->deleteLater();
    m_running = false;
    m_lock.release();

    m_bar->setRange(0, 100);
    if (result.cancelled) {
        m_hasResult = false;
        m_result = {};
        m_model->setEntries({});
        m_problems->clear();
        m_counts->setText(emptyCounts());
        m_bar->setValue(0);
        m_state->setFullText(QStringLiteral("已取消"));
        m_services.logWarning(QStringLiteral("比较已取消"));
        m_services.showStatus(StatusState::Info, QStringLiteral("比较已取消"));
    } else if (!error.isEmpty()) {
        m_hasResult = false;
        m_result = {};
        m_model->setEntries({});
        m_problems->clear();
        m_counts->setText(emptyCounts());
        m_bar->setValue(0);
        showFailure(error);
    } else {
        m_hasResult = true;
        m_comparedA = m_activeA;
        m_comparedB = m_activeB;
        m_result = result;
        m_model->setEntries(result.entries);
        m_problems->setPlainText(formatProblems(result));
        m_counts->setText(formatCounts(result));
        m_bar->setValue(100);
        const QString summary = formatCounts(result);
        QString state = QStringLiteral("比较完成");
        if (result.truncated)
            state = QStringLiteral("比较完成，扫描达到上限，结果不完整");
        else if (result.problemCount > 0)
            state = QStringLiteral("比较完成，有 %1 项问题，请查看下方列表").arg(result.problemCount);
        m_state->setFullText(state);
        const bool needsAttention = result.problemCount > 0 || result.truncated;
        if (needsAttention)
            m_services.logWarning(state + QStringLiteral("。") + summary);
        else
            m_services.logSuccess(summary);
        m_services.showStatus(needsAttention ? StatusState::Info : StatusState::Success,
                              needsAttention ? state : summary);
    }
    applyFilter();
    updateActions();
}

void Md5ComparePage::onExportFinished()
{
    if (!m_exportThread)
        return;
    const bool ok = m_exportThread->ok();
    const QString error = m_exportThread->error();
    ExportThread *thread = m_exportThread;
    m_exportThread = nullptr;
    thread->deleteLater();
    m_running = false;
    m_lock.release();

    m_bar->setRange(0, 100);
    if (ok) {
        m_bar->setValue(100);
        m_state->setFullText(QStringLiteral("已导出报告"));
        m_services.logSuccess(QStringLiteral("已导出报告"));
        m_services.showStatus(StatusState::Success, QStringLiteral("已导出报告"));
    } else if (m_cancelRequested.load(std::memory_order_acquire)) {
        m_bar->setValue(0);
        m_state->setFullText(QStringLiteral("导出已取消，比较结果还在"));
        m_services.logWarning(QStringLiteral("导出已取消"));
        m_services.showStatus(StatusState::Info, QStringLiteral("导出已取消"));
    } else {
        m_bar->setValue(0);
        showFailure(error.isEmpty() ? QStringLiteral("无法写入报告") : error);
    }
    updateActions();
}

void Md5ComparePage::applyFilter()
{
    bool ok = false;
    const int filter = m_filter->currentData().toInt(&ok);
    m_model->setFilter(ok ? filter : -1, m_search->text());
    m_visible->setText(QStringLiteral("当前显示 %1 条").arg(m_model->rowCount()));
}

void Md5ComparePage::updateActions()
{
    m_start->setEnabled(!m_running);
    m_cancel->setEnabled(m_running);
    m_browseA->setEnabled(!m_running);
    m_browseB->setEnabled(!m_running);
    m_dirA->setEnabled(!m_running);
    m_dirB->setEnabled(!m_running);
    m_export->setEnabled(!m_running && m_hasResult);
}

void Md5ComparePage::loadState()
{
    const QString file = statePath();
    if (file.isEmpty() || !QFileInfo::exists(file))
        return;
    QSettings settings(file, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    const QString dirA = settings.value(QStringLiteral("dirA")).toString();
    const QString dirB = settings.value(QStringLiteral("dirB")).toString();
    if (!dirA.isEmpty())
        m_dirA->setText(dirA);
    if (!dirB.isEmpty())
        m_dirB->setText(dirB);
}

void Md5ComparePage::saveState()
{
    const QString file = statePath();
    if (file.isEmpty())
        return;
    QSettings settings(file, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    settings.setValue(QStringLiteral("dirA"), m_dirA->text());
    settings.setValue(QStringLiteral("dirB"), m_dirB->text());
    settings.sync();
}

QString Md5ComparePage::statePath() const
{
    QString error;
    const QString dir = m_services.dataDir(&error);
    if (dir.isEmpty())
        return {};
    return QDir(dir).filePath(QStringLiteral("state.ini"));
}

void Md5ComparePage::showFailure(const QString &message)
{
    m_state->setFullText(message);
    m_services.logError(message);
    m_services.showStatus(StatusState::Error, message);
}

ToolInfo Md5CompareTool::info() const
{
    // 源码目录是 md5_compare。平台标识不允许下划线，注册 id 固定为 md5-compare。
    ToolInfo info;
    info.id = QStringLiteral("md5-compare");
    info.title = QStringLiteral("目录 MD5 比较");
    info.summary = QStringLiteral("按相对路径递归比较两个目录中普通文件的 MD5");
    info.iconKey = QStringLiteral("status");
    info.order = 40;
    return info;
}

bool Md5CompareTool::initialize(const ToolContext &context, QString *error)
{
    m_context = context;
    QString dirError;
    if (m_context.services.dataDir(&dirError).isEmpty()) {
        if (error)
            *error = dirError.isEmpty() ? QStringLiteral("无法准备工具数据目录") : dirError;
        return false;
    }
    return true;
}

QVector<ToolPage> Md5CompareTool::createPages()
{
    auto *page = new Md5ComparePage(m_context.services);
    m_page = page;
    QVector<ToolPage> pages;
    pages.append({PageInfo{QStringLiteral("compare"), QStringLiteral("比较"),
                           QStringLiteral("选择两个目录并查看 MD5 比较结果"), QStringLiteral("status")},
                  page});
    return pages;
}

void Md5CompareTool::shutdown()
{
    if (m_page)
        m_page->abort();
}

} // namespace tw

#include "md5_compare_tool.moc"
