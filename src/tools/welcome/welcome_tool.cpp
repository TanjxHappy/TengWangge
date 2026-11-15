#include "tools/welcome/welcome_tool.h"

#include "core/app_info.h"

#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QVariant>

namespace tw {
namespace {

QLabel *makeLabel(const QString &text, const char *role, QWidget *parent = nullptr)
{
    auto *label = new QLabel(text, parent);
    label->setProperty("role", QString::fromLatin1(role));
    label->setWordWrap(true);
    return label;
}

QWidget *makeOverviewPage(const QString &dataDir, const QString &dataDirError)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);

    layout->addWidget(makeLabel(QStringLiteral("欢迎使用%1").arg(AppInfo::productName()), "pageTitle"));
    layout->addWidget(makeLabel(
        QStringLiteral("滕王阁是一个“外壳 + 工具模块”的桌面工具箱平台。左侧选择工具，"
                       "工具顶部的页面条切换该工具内部的页面。"),
        "pageSubtitle"));

    auto *card = new QFrame;
    card->setProperty("role", QStringLiteral("card"));
    auto *grid = new QGridLayout(card);
    grid->setContentsMargins(16, 14, 16, 14);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(10);
    grid->setColumnStretch(1, 1);

    auto addRow = [&](int row, const QString &name, const QString &value) {
        grid->addWidget(makeLabel(name, "fieldLabel"), row, 0, Qt::AlignTop);
        QLabel *v = makeLabel(value, "value");
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(v, row, 1);
    };
    addRow(0, QStringLiteral("版本"), AppInfo::version());
    addRow(1, QStringLiteral("程序名"), AppInfo::internalName());
    addRow(2, QStringLiteral("本工具数据目录"),
           dataDirError.isEmpty() ? dataDir : QStringLiteral("不可用：%1").arg(dataDirError));
    layout->addWidget(card);

    layout->addWidget(makeLabel(
        QStringLiteral("所有数据只写入上面这个隔离目录之下，不会触碰系统目录。"
                       "可用环境变量 TENGWANGGE_DATA_DIR 指定其他位置。"),
        "hint"));
    layout->addStretch(1);
    return page;
}

QWidget *makeKeysPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);
    layout->addWidget(makeLabel(QStringLiteral("键盘操作"), "pageTitle"));
    layout->addWidget(makeLabel(QStringLiteral("所有工具共用同一套导航快捷键。"), "pageSubtitle"));

    auto *card = new QFrame;
    card->setProperty("role", QStringLiteral("card"));
    auto *grid = new QGridLayout(card);
    grid->setContentsMargins(16, 14, 16, 14);
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(1, 1);

    const QList<QPair<QString, QString>> rows = {
        {QStringLiteral("Ctrl+1 … Ctrl+9"), QStringLiteral("切换到第 N 个工具")},
        {QStringLiteral("Alt+↑ / Alt+↓"), QStringLiteral("上一个 / 下一个工具")},
        {QStringLiteral("Ctrl+PageUp / PageDown"), QStringLiteral("当前工具的上一页 / 下一页")},
        {QStringLiteral("F6"), QStringLiteral("在工具栏、页面条、页面、日志之间移动焦点")},
        {QStringLiteral("Ctrl+L"), QStringLiteral("显示 / 隐藏日志面板")},
        {QStringLiteral("F1"), QStringLiteral("弹出键盘操作帮助")},
    };
    int row = 0;
    for (const auto &entry : rows) {
        grid->addWidget(makeLabel(entry.first, "value"), row, 0);
        grid->addWidget(makeLabel(entry.second, "pageSubtitle"), row, 1);
        ++row;
    }
    layout->addWidget(card);
    layout->addWidget(makeLabel(
        QStringLiteral("当前工具和页面始终有明确的高亮：工具栏左侧的琥珀色竖条，页面条下方的琥珀色下划线。"
                       "长时间任务进行时导航整体变灰并在状态栏提示原因。"),
        "hint"));
    layout->addStretch(1);
    return page;
}

} // namespace

ToolInfo WelcomeTool::info() const
{
    ToolInfo info;
    info.id = QStringLiteral("welcome");
    info.title = QStringLiteral("概览");
    info.summary = QStringLiteral("平台介绍与键盘操作");
    info.iconKey = QStringLiteral("overview");
    info.order = 10;
    return info;
}

bool WelcomeTool::initialize(const ToolContext &context, QString *)
{
    m_context = context;
    return true;
}

QVector<ToolPage> WelcomeTool::createPages()
{
    QString error;
    const QString dir = m_context.services.dataDir(&error);

    QVector<ToolPage> pages;
    pages.append({PageInfo{QStringLiteral("overview"), QStringLiteral("平台概览"),
                           QStringLiteral("版本与数据目录"), QStringLiteral("overview")},
                  makeOverviewPage(dir, error)});
    pages.append({PageInfo{QStringLiteral("keys"), QStringLiteral("键盘操作"),
                           QStringLiteral("导航快捷键一览"), QString()},
                  makeKeysPage()});
    return pages;
}

void WelcomeTool::activated()
{
    m_context.services.showStatus(StatusState::Info, QStringLiteral("欢迎使用%1").arg(AppInfo::productName()));
}

} // namespace tw
