#pragma once

#include "tools/core_analyze/command_runner.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

namespace tw {

// 把系统里的 file / readelf / eu-unstrip / gdb 串成一份给非开发人员看的说明。
// 解析和措辞是本工具自己的；不内嵌第三方 core 解析器。
struct AnalysisRequest
{
    QString corePath;
    QString executablePath;
    QString symbolPath;
    QString workDirectory;
};

struct ExecutedStep
{
    QString title;
    QString purpose;
    QString commandDisplay;
    QString statusText;
    QString output;
    QString interpretation;
    bool ran = false;
};

struct AnalysisReport
{
    bool ok = false;
    bool isCore = false;
    bool cancelled = false;
    int signalNumber = 0;
    QString signalName;
    QString errorInfo;
    QString whereInfo;
    QString cause;
    QString suggestionText;
    QVector<ExecutedStep> steps;
    QStringList limits;
    QString rendered;
};

using ToolFinder = std::function<QString(const QString &name)>;

class CoreAnalyzer
{
public:
    // 还没选文件时也能看的操作说明。
    static QString usageGuide();

    // 只用于把已经拆开的参数显示成一行，不参与真正执行。
    static QString formatCommand(const QString &program, const QStringList &arguments);

    // finder 为空时用 PATH 查找。cancel 可为空。progress 可为空。
    static AnalysisReport analyze(const AnalysisRequest &request,
                                  CommandExecutor &executor,
                                  const ToolFinder &finder = {},
                                  const std::atomic<bool> *cancel = nullptr,
                                  const std::function<void(const QString &)> &progress = {});
};

} // namespace tw
