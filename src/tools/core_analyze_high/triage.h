#pragma once

#include "tools/core_analyze_high/launch.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

namespace tw {

struct TriageInput
{
    QString corePath;
    QString programPath;
    QString debugPath;
    QString directory;
};

struct TriageStep
{
    QString heading;
    QString purpose;
    QString commandLine;
    QString situation;
    QString excerpt;
    QString meaning;
    bool invoked = false;
};

struct TriageOutcome
{
    bool accepted = false;
    bool coreFile = false;
    bool stoppedEarly = false;
    int signalCode = 0;
    QString signalToken;
    QString fault;
    QString place;
    QString likely;
    QString advice;
    QVector<TriageStep> steps;
    QStringList caveats;
    QString document;
};

// 按名字在 PATH 里找可执行文件。测试可以换成固定路径。
using ToolLocator = std::function<QString(const QString &name)>;

// 把 file / eu-readelf / eu-unstrip / gdb 的结果写成非开发人员能读的说明。
// 信号和调用栈来自这些外部程序，本类只负责编排和翻译，不内嵌第三方解析库。
class CoreTriage
{
public:
    static QString primer();

    static TriageOutcome run(const TriageInput &input, ArgvHost &host,
                             const ToolLocator &locator = {},
                             const std::atomic<bool> *stop = nullptr,
                             const std::function<void(const QString &)> &note = {});
};

} // namespace tw
