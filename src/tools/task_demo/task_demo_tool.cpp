#include "tools/task_demo/task_demo_tool.h"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

namespace tw {
namespace {

QLabel *makeLabel(const QString &text, const char *role)
{
    auto *label = new QLabel(text);
    label->setProperty("role", QString::fromLatin1(role));
    label->setWordWrap(true);
    return label;
}

} // namespace

TaskDemoPage::TaskDemoPage(const ToolServices &services, int stepIntervalMs, QWidget *parent)
    : QWidget(parent)
    , m_services(services)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);

    layout->addWidget(makeLabel(QStringLiteral("演示任务"), "pageTitle"));
    layout->addWidget(makeLabel(
        QStringLiteral("模拟一次长时间任务。任务运行时导航被锁定：不能切换工具或页面，"
                       "状态栏显示原因；结束或取消后自动恢复。"),
        "pageSubtitle"));

    auto *card = new QFrame;
    card->setProperty("role", QStringLiteral("card"));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(16, 14, 16, 14);
    cardLayout->setSpacing(12);

    m_state = makeLabel(QStringLiteral("尚未运行"), "sectionTitle");
    m_bar = new QProgressBar;
    m_bar->setRange(0, 100);
    m_bar->setTextVisible(true);
    m_bar->setAccessibleName(QStringLiteral("任务进度"));
    m_fail = new QCheckBox(QStringLiteral("让任务在 60% 处失败（演示错误状态）"));

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    m_start = new QPushButton(QStringLiteral("开始任务"));
    m_start->setProperty("role", QStringLiteral("primary"));
    m_start->setDefault(true);
    m_cancel = new QPushButton(QStringLiteral("取消"));
    buttons->addWidget(m_start);
    buttons->addWidget(m_cancel);
    buttons->addStretch(1);

    cardLayout->addWidget(m_state);
    cardLayout->addWidget(m_bar);
    cardLayout->addWidget(m_fail);
    cardLayout->addLayout(buttons);
    layout->addWidget(card);
    layout->addStretch(1);

    m_timer = new QTimer(this);
    m_timer->setInterval(qMax(1, stepIntervalMs));
    connect(m_timer, &QTimer::timeout, this, &TaskDemoPage::tick);
    connect(m_start, &QPushButton::clicked, this, &TaskDemoPage::start);
    connect(m_cancel, &QPushButton::clicked, this, &TaskDemoPage::cancel);

    setTabOrder(m_fail, m_start);
    setTabOrder(m_start, m_cancel);
    updateUi();
}

void TaskDemoPage::start()
{
    if (m_running)
        return;
    m_running = true;
    m_progress = 0;
    m_lock = m_services.lockNavigation(QStringLiteral("演示任务运行中"));
    m_services.logInfo(QStringLiteral("演示任务开始"));
    m_services.showStatus(StatusState::Busy, QStringLiteral("演示任务运行中 0%"));
    m_timer->start();
    updateUi();
}

void TaskDemoPage::cancel()
{
    if (!m_running)
        return;
    m_timer->stop();
    m_running = false;
    m_lock.release();
    m_services.logWarning(QStringLiteral("演示任务已取消（进度 %1%）").arg(m_progress));
    m_services.showStatus(StatusState::Info, QStringLiteral("演示任务已取消"));
    m_state->setText(QStringLiteral("已取消"));
    updateUi();
    emit finished(false);
}

void TaskDemoPage::tick()
{
    m_progress = qMin(100, m_progress + 10);
    if (m_fail->isChecked() && m_progress >= 60) {
        end(false, QStringLiteral("演示任务失败：模拟的错误（进度 %1%）").arg(m_progress));
        return;
    }
    if (m_progress >= 100) {
        end(true, QStringLiteral("演示任务完成"));
        return;
    }
    m_services.showStatus(StatusState::Busy, QStringLiteral("演示任务运行中 %1%").arg(m_progress));
    updateUi();
}

void TaskDemoPage::end(bool success, const QString &message)
{
    m_timer->stop();
    m_running = false;
    m_lock.release();
    if (success) {
        m_services.logSuccess(message);
        m_services.showStatus(StatusState::Success, message);
        m_state->setText(QStringLiteral("已完成"));
    } else {
        m_services.logError(message);
        m_services.showStatus(StatusState::Error, message);
        m_state->setText(QStringLiteral("失败"));
    }
    updateUi();
    emit finished(success);
}

void TaskDemoPage::abort()
{
    m_timer->stop();
    m_running = false;
    m_lock.release();
}

void TaskDemoPage::updateUi()
{
    m_bar->setValue(m_progress);
    if (m_running)
        m_state->setText(QStringLiteral("运行中"));
    m_start->setEnabled(!m_running);
    m_cancel->setEnabled(m_running);
    m_fail->setEnabled(!m_running);
}

// ---------------------------------------------------------------------------

TaskDemoTool::TaskDemoTool(int stepIntervalMs)
    : m_stepIntervalMs(stepIntervalMs)
{
}

ToolInfo TaskDemoTool::info() const
{
    ToolInfo info;
    info.id = QStringLiteral("task-demo");
    info.title = QStringLiteral("任务演示");
    info.summary = QStringLiteral("演示长时间任务、状态栏与导航锁定");
    info.iconKey = QStringLiteral("task");
    info.order = 20;
    return info;
}

bool TaskDemoTool::initialize(const ToolContext &context, QString *)
{
    m_context = context;
    return true;
}

QVector<ToolPage> TaskDemoTool::createPages()
{
    auto *page = new TaskDemoPage(m_context.services, m_stepIntervalMs);
    m_page = page;

    auto *about = new QWidget;
    auto *layout = new QVBoxLayout(about);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);
    layout->addWidget(makeLabel(QStringLiteral("关于本工具"), "pageTitle"));
    layout->addWidget(makeLabel(
        QStringLiteral("这是一个没有业务功能的示例工具，用来展示新工具如何使用外壳服务："
                       "日志、状态栏、导航锁，以及一个工具提供多个页面。"),
        "pageSubtitle"));
    layout->addWidget(makeLabel(
        QStringLiteral("任务运行时切到其他工具会被拒绝；任务页自己的“取消”按钮始终可用。"),
        "hint"));
    layout->addStretch(1);

    QVector<ToolPage> pages;
    pages.append({PageInfo{QStringLiteral("run"), QStringLiteral("任务"),
                           QStringLiteral("运行演示任务"), QStringLiteral("task")},
                  page});
    pages.append({PageInfo{QStringLiteral("about"), QStringLiteral("说明"),
                           QStringLiteral("本工具演示的内容"), QStringLiteral("info")},
                  about});
    return pages;
}

void TaskDemoTool::shutdown()
{
    if (m_page)
        m_page->abort();
}

} // namespace tw
