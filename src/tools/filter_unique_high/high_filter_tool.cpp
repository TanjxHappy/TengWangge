#include "tools/filter_unique_high/high_filter_tool.h"

namespace tw {

HighFilterTool::~HighFilterTool()
{
    shutdown();
}

ToolInfo HighFilterTool::info() const
{
    ToolInfo info;
    info.id = QStringLiteral("filter-unique-high");
    info.title = QStringLiteral("筛选去重high");
    info.summary = QStringLiteral("按名单对目录做不区分大小写的子串筛选，确认后才删除");
    info.iconKey = QStringLiteral("status");
    info.order = 40;
    return info;
}

bool HighFilterTool::initialize(const ToolContext &context, QString *error)
{
    m_context = context;
    QString dirError;
    m_dataDir = context.services.dataDir(&dirError);
    if (m_dataDir.isEmpty()) {
        if (error)
            *error = dirError.isEmpty() ? QStringLiteral("数据目录不可用") : dirError;
        return false;
    }
    return true;
}

QVector<ToolPage> HighFilterTool::createPages()
{
    auto *page = new HighFilterPage(m_context.services, m_dataDir, m_context.dialogParent);
    m_page = page;

    QVector<ToolPage> pages;
    pages.append({PageInfo{QStringLiteral("board"), QStringLiteral("筛选"),
                           QStringLiteral("按名单筛选目录中的文件"), QStringLiteral("status")},
                  page});
    return pages;
}

void HighFilterTool::shutdown()
{
    if (m_page)
        m_page->abort();
}

} // namespace tw
