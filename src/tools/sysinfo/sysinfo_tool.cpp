#include "tools/sysinfo/sysinfo_tool.h"

#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QScreen>
#include <QSysInfo>
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

QString screenSummary()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
        return QStringLiteral("未检测到");
    const QSize size = screen->size();
    return QStringLiteral("%1 × %2，缩放 %3")
        .arg(size.width())
        .arg(size.height())
        .arg(screen->devicePixelRatio());
}

} // namespace

ToolInfo SysInfoTool::info() const
{
    ToolInfo info;
    info.id = QStringLiteral("sysinfo");
    info.title = QStringLiteral("本机信息");
    info.summary = QStringLiteral("操作系统、Qt 运行时与显示环境");
    info.iconKey = QStringLiteral("environment");
    info.order = 30;
    return info;
}

bool SysInfoTool::initialize(const ToolContext &context, QString *)
{
    m_context = context;
    return true;
}

QVector<ToolPage> SysInfoTool::createPages()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);
    layout->addWidget(makeLabel(QStringLiteral("本机信息"), "pageTitle"));
    layout->addWidget(makeLabel(QStringLiteral("只读信息，用于排查环境问题。"), "pageSubtitle"));

    auto *card = new QFrame;
    card->setProperty("role", QStringLiteral("card"));
    auto *grid = new QGridLayout(card);
    grid->setContentsMargins(16, 14, 16, 14);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(10);
    grid->setColumnStretch(1, 1);

    const QList<QPair<QString, QString>> rows = {
        {QStringLiteral("操作系统"), QSysInfo::prettyProductName()},
        {QStringLiteral("内核"), QSysInfo::kernelType() + QLatin1Char(' ') + QSysInfo::kernelVersion()},
        {QStringLiteral("CPU 架构"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("主机名"), QSysInfo::machineHostName()},
        {QStringLiteral("Qt 运行时"), QString::fromLatin1(qVersion())},
        {QStringLiteral("Qt 编译版本"), QStringLiteral(QT_VERSION_STR)},
        {QStringLiteral("显示后端"), QGuiApplication::platformName()},
        {QStringLiteral("主屏幕"), screenSummary()},
    };
    int row = 0;
    for (const auto &entry : rows) {
        grid->addWidget(makeLabel(entry.first, "fieldLabel"), row, 0, Qt::AlignTop);
        QLabel *value = makeLabel(entry.second, "value");
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(value, row, 1);
        ++row;
    }
    layout->addWidget(card);
    layout->addStretch(1);

    QVector<ToolPage> pages;
    pages.append({PageInfo{QStringLiteral("system"), QStringLiteral("系统"),
                           QStringLiteral("操作系统与 Qt 版本"), QStringLiteral("environment")},
                  page});
    return pages;
}

void SysInfoTool::activated()
{
    m_context.services.showStatus(StatusState::Info, QStringLiteral("已读取本机信息"));
}

} // namespace tw
