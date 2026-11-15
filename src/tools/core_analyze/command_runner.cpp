#include "tools/core_analyze/command_runner.h"

#include <QDir>
#include <QElapsedTimer>
#include <QProcess>

namespace tw {
namespace {

constexpr int kOutputCapBytes = 64 * 1024;

QString decodeOutput(const QByteArray &bytes)
{
    return QString::fromUtf8(bytes);
}

} // namespace

ProcessExecutor::ProcessExecutor(std::atomic<bool> *cancel)
    : m_cancel(cancel)
{
}

CommandResult ProcessExecutor::run(const CommandRequest &request)
{
    CommandResult result;
    if (m_cancel && m_cancel->load()) {
        result.status = CommandStatus::Cancelled;
        result.errorDetail = QStringLiteral("已取消");
        return result;
    }
    if (request.program.isEmpty()) {
        result.status = CommandStatus::MissingProgram;
        result.errorDetail = QStringLiteral("没有可执行文件");
        return result;
    }

    QProcess process;
    process.setProgram(request.program);
    process.setArguments(request.arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setStandardInputFile(QProcess::nullDevice());
    if (!request.workDirectory.isEmpty())
        process.setWorkingDirectory(request.workDirectory);

    // 固定英文消息，避免不同语言的翻译把关键字拆散；编码仍沿用系统环境。
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("LC_ALL"));
    env.insert(QStringLiteral("LANGUAGE"), QStringLiteral("en"));
    env.insert(QStringLiteral("LC_MESSAGES"), QStringLiteral("C"));
    env.insert(QStringLiteral("DEBUGINFOD_URLS"), QString());
    process.setProcessEnvironment(env);

    process.start();
    if (!process.waitForStarted(3000)) {
        if (m_cancel && m_cancel->load()) {
            result.status = CommandStatus::Cancelled;
            result.errorDetail = QStringLiteral("已取消");
            return result;
        }
        result.status = CommandStatus::FailedToStart;
        result.errorDetail = process.errorString();
        return result;
    }

    QElapsedTimer timer;
    timer.start();
    QByteArray collected;
    bool timedOut = false;
    bool cancelled = false;
    while (!process.waitForFinished(100)) {
        collected += process.readAllStandardOutput();
        collected += process.readAllStandardError();
        if (collected.size() > kOutputCapBytes) {
            result.truncated = true;
            process.kill();
            process.waitForFinished(2000);
            break;
        }
        if (m_cancel && m_cancel->load()) {
            cancelled = true;
            process.kill();
            process.waitForFinished(2000);
            break;
        }
        if (timer.elapsed() > request.timeoutMs) {
            timedOut = true;
            process.kill();
            process.waitForFinished(2000);
            break;
        }
    }
    collected += process.readAllStandardOutput();
    collected += process.readAllStandardError();
    if (collected.size() > kOutputCapBytes) {
        collected.truncate(kOutputCapBytes);
        result.truncated = true;
    }
    result.output = decodeOutput(collected);
    result.exitCode = process.exitCode();

    if (cancelled) {
        result.status = CommandStatus::Cancelled;
        result.errorDetail = QStringLiteral("已取消");
        return result;
    }
    if (timedOut) {
        result.status = CommandStatus::TimedOut;
        result.errorDetail = QStringLiteral("超过 %1 秒仍未结束").arg(request.timeoutMs / 1000);
        return result;
    }
    result.status = CommandStatus::Ran;
    return result;
}

} // namespace tw
