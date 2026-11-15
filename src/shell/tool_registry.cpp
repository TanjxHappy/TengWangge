#include "shell/tool_registry.h"

#include <algorithm>

namespace tw {

bool ToolRegistry::add(std::unique_ptr<ITool> tool, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    if (!tool)
        return fail(QStringLiteral("工具对象为空"));
    const ToolInfo info = tool->info();
    if (!isValidIdentifier(info.id))
        return fail(QStringLiteral("工具标识无效：%1").arg(info.id));
    if (info.title.trimmed().isEmpty())
        return fail(QStringLiteral("工具 %1 缺少标题").arg(info.id));
    if (indexOf(info.id) >= 0)
        return fail(QStringLiteral("工具标识重复：%1").arg(info.id));

    const auto position = std::upper_bound(
        m_entries.begin(), m_entries.end(), info.order,
        [](int order, const Entry &entry) { return order < entry.info.order; });
    m_entries.insert(position, Entry{std::move(tool), info});
    return true;
}

ITool *ToolRegistry::at(int index) const
{
    if (index < 0 || index >= count())
        return nullptr;
    return m_entries[static_cast<size_t>(index)].tool.get();
}

ToolInfo ToolRegistry::infoAt(int index) const
{
    if (index < 0 || index >= count())
        return {};
    return m_entries[static_cast<size_t>(index)].info;
}

int ToolRegistry::indexOf(const QString &id) const
{
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].info.id == id)
            return static_cast<int>(i);
    }
    return -1;
}

ITool *ToolRegistry::find(const QString &id) const
{
    return at(indexOf(id));
}

} // namespace tw
