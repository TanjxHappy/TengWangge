#include "tools/param_check_high/check_high_tool.h"

#include "tools/param_check_high/check_high_page.h"

namespace tw {

ToolInfo ParamCheckHighTool::info() const
{
    ToolInfo info;
    // 标识会进入目录名。平台只接受小写字母、数字，以及用 . 或 - 分段。
    info.id = QStringLiteral("param-check-high");
    info.title = QStringLiteral("参数检查high");
    info.summary = QStringLiteral("左右对照检查并修改 JSON、XML、INI 参数文件");
    info.iconKey = QStringLiteral("status");
    info.order = 41;
    return info;
}

bool ParamCheckHighTool::initialize(const ToolContext &context, QString *error)
{
    m_context = context;
    QString dirError;
    if (m_context.services.dataDir(&dirError).isEmpty()) {
        if (error)
            *error = dirError;
        return false;
    }
    return true;
}

QVector<ToolPage> ParamCheckHighTool::createPages()
{
    auto *page = new HighCheckPage(m_context.services);
    m_page = page;

    QVector<ToolPage> pages;
    pages.append({PageInfo{QStringLiteral("editor"), QStringLiteral("参数检查high"),
                           QStringLiteral("查看并修改参数文件"), QStringLiteral("status")},
                  page});
    return pages;
}

void ParamCheckHighTool::activated()
{
    if (m_page && !m_page->parseError().isEmpty())
        m_context.services.showStatus(StatusState::Error, m_page->parseError());
}

void ParamCheckHighTool::shutdown()
{
    if (m_page)
        m_page->persistSession();
}

HighCheckPage *ParamCheckHighTool::page() const
{
    return m_page.data();
}

} // namespace tw
