#pragma once

#include <QString>

namespace tw {

// Product identity in one place. Everything that names the app (window title,
// data directory, desktop entry) derives from here.
struct AppInfo
{
    static QString productName();  // 显示名：滕王阁
    static QString internalName(); // 程序与目录名：TengWangge
    static QString version();      // 来自 CMake project(VERSION)
    static QString dataDirName();  // 隔离数据目录名，不与其他产品共用
};

} // namespace tw
