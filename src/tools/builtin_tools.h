#pragma once

#include "shell/tool_registry.h"

namespace tw {

// 编译期接入的内置工具清单。显式列出，不依赖静态初始化顺序。
// 返回值：全部注册成功为 true；失败原因写入 error。
bool registerBuiltinTools(ToolRegistry &registry, QString *error = nullptr);

} // namespace tw
