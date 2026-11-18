#include "tools/param_check/param_check_tool.h"

#include "tools/param_check/param_editor_page.h"

namespace tw {

ToolInfo ParamCheckTool::info() const
{
    ToolInfo info;
    // 标识会进入目录名。平台只接受小写字母、数字，以及用 . 或 - 分段，
    // 下划线会被 isValidIdentifier 拒绝（core 测试把 a_b 定为非法）。
    // 因此稳定 id 用对外名称 param-check，源码目录仍是 param_check。
    info.id = QStringLiteral("param-check");
    info.title = QStringLiteral("参数检查");
    info.summary = QStringLiteral("可视化检查并编辑 JSON、XML、INI 参数文件");
    info.iconKey = QStringLiteral("info");
    info.order = 40;
    return info;
}

bool ParamCheckTool::initialize(const ToolContext &context, QString *error)
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

QVector<ToolPage> ParamCheckTool::createPages()
{
    auto *page = new ParamEditorPage(m_context.services);
    m_page = page;

    QVector<ToolPage> pages;
    pages.append({PageInfo{QStringLiteral("editor"), QStringLiteral("参数检查"),
                           QStringLiteral("查看并编辑参数文件"), QStringLiteral("info")},
                  page});
    return pages;
}

void ParamCheckTool::activated()
{
    if (m_page && !m_page->parseError().isEmpty())
        m_context.services.showStatus(StatusState::Error, m_page->parseError());
}

void ParamCheckTool::shutdown()
{
    if (m_page)
        m_page->persistSession();
}

ParamEditorPage *ParamCheckTool::page() const
{
    return m_page.data();
}

} // namespace tw
