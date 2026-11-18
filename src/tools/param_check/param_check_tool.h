#pragma once

#include "shell/i_tool.h"

#include <QPointer>

namespace tw {

class ParamEditorPage;

// 参数检查。稳定 id 见 info()：平台标识不能含下划线，所以用 param-check。
// 源码目录是 param_check，工具栏名称是「参数检查」。
class ParamCheckTool : public ITool
{
public:
    ToolInfo info() const override;
    bool initialize(const ToolContext &context, QString *error) override;
    QVector<ToolPage> createPages() override;
    void activated() override;
    void shutdown() override;

    ParamEditorPage *page() const;

private:
    ToolContext m_context;
    QPointer<ParamEditorPage> m_page;
};

} // namespace tw
