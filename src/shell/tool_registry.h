#pragma once

#include "shell/i_tool.h"

#include <memory>
#include <vector>

namespace tw {

// 持有所有已注册工具。注册时校验标识和标题，按 ToolInfo::order 稳定排序。
// 外壳不认识任何具体工具，工具列表由应用层（builtin_tools）或测试注入。
class ToolRegistry
{
public:
    ToolRegistry() = default;
    ToolRegistry(ToolRegistry &&) = default;
    ToolRegistry &operator=(ToolRegistry &&) = default;
    ToolRegistry(const ToolRegistry &) = delete;
    ToolRegistry &operator=(const ToolRegistry &) = delete;

    // 成功返回 true；标识无效、标题为空或标识重复时拒绝并写入 error。
    bool add(std::unique_ptr<ITool> tool, QString *error = nullptr);

    int count() const { return static_cast<int>(m_entries.size()); }
    ITool *at(int index) const;
    ToolInfo infoAt(int index) const;
    int indexOf(const QString &id) const;
    ITool *find(const QString &id) const;

private:
    struct Entry {
        std::unique_ptr<ITool> tool;
        ToolInfo info;
    };
    std::vector<Entry> m_entries;
};

} // namespace tw
