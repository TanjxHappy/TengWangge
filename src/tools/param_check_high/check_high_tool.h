#pragma once

#include "shell/i_tool.h"

#include <QPointer>

namespace tw {

class HighCheckPage;

// 参数检查 high。稳定 id 是 param-check-high：平台标识不能含下划线。
// 源码目录是 param_check_high，工具栏名称是「参数检查high」。
class ParamCheckHighTool : public ITool
{
public:
    ToolInfo info() const override;
    bool initialize(const ToolContext &context, QString *error) override;
    QVector<ToolPage> createPages() override;
    void activated() override;
    void shutdown() override;

    HighCheckPage *page() const;

private:
    ToolContext m_context;
    QPointer<HighCheckPage> m_page;
};

} // namespace tw
