#pragma once

#include "shell/i_tool.h"

namespace tw {

// 内置示例工具：只读的本机信息，单页面工具的参考实现。
class SysInfoTool : public ITool
{
public:
    ToolInfo info() const override;
    bool initialize(const ToolContext &context, QString *error) override;
    QVector<ToolPage> createPages() override;
    void activated() override;

private:
    ToolContext m_context;
};

} // namespace tw
