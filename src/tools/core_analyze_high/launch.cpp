#include "tools/core_analyze_high/launch.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

namespace tw {
namespace {

constexpr int kCaptureCap = 48 * 1024;

bool rawTokenIsSafe(const QString &token)
{
    if (token.isEmpty())
        return false;
    for (const QChar ch : token) {
        const ushort code = ch.unicode();
        const bool ok = (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z')
            || (code >= '0' && code <= '9') || ch == QLatin1Char('_') || ch == QLatin1Char('.')
            || ch == QLatin1Char('/') || ch == QLatin1Char(':') || ch == QLatin1Char('+')
            || ch == QLatin1Char('@') || ch == QLatin1Char('%') || ch == QLatin1Char('=')
            || ch == QLatin1Char(',') || ch == QLatin1Char('-');
        if (!ok)
            return false;
    }
    return true;
}

QString quoteToken(const QString &token)
{
    if (rawTokenIsSafe(token))
        return token;
    QString escaped = token;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'%1'").arg(escaped);
}

} // namespace

SystemArgvHost::SystemArgvHost(std::atomic<bool> *stop)
    : m_stop(stop)
{
}

ArgvResult SystemArgvHost::run(const ArgvTask &task)
{
    ArgvResult result;
    if (m_stop && m_stop->load()) {
        result.state = ArgvState::Stopped;
        result.detail = QStringLiteral("已取消");
        return result;
    }

    const QFileInfo programInfo(task.program);
    if (task.program.isEmpty() || !programInfo.exists() || !programInfo.isExecutable()) {
        result.state = ArgvState::NotFound;
        result.detail = QStringLiteral("没有可执行文件");
        return result;
    }

    QProcess process;
    process.setProgram(task.program);
    process.setArguments(task.arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setStandardInputFile(QProcess::nullDevice());
    if (!task.directory.isEmpty())
        process.setWorkingDirectory(task.directory);

    // 子进程用英文消息，关键字才稳定；路径字节仍由当前进程的本地编码传递。
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("LC_ALL"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    environment.insert(QStringLiteral("LANGUAGE"), QStringLiteral("C"));
    environment.insert(QStringLiteral("LC_MESSAGES"), QStringLiteral("C"));
    environment.insert(QStringLiteral("DEBUGINFOD_URLS"), QString());
    process.setProcessEnvironment(environment);

    process.start();
    if (!process.waitForStarted(3000)) {
        if (m_stop && m_stop->load()) {
            result.state = ArgvState::Stopped;
            result.detail = QStringLiteral("已取消");
            return result;
        }
        result.state = ArgvState::StartFailed;
        result.detail = process.errorString();
        return result;
    }

    QElapsedTimer timer;
    timer.start();
    QByteArray captured;
    bool timedOut = false;
    bool stopped = false;
    bool capped = false;

    auto drain = [&]() {
        captured += process.readAllStandardOutput();
        captured += process.readAllStandardError();
    };

    while (process.state() != QProcess::NotRunning) {
        if (m_stop && m_stop->load()) {
            stopped = true;
            process.kill();
            process.waitForFinished(2000);
            break;
        }
        if (timer.elapsed() > task.timeoutMs) {
            timedOut = true;
            process.kill();
            process.waitForFinished(2000);
            break;
        }
        process.waitForFinished(100);
        drain();
        if (captured.size() > kCaptureCap) {
            capped = true;
            process.kill();
            process.waitForFinished(2000);
            break;
        }
    }
    drain();
    if (captured.size() > kCaptureCap) {
        captured.truncate(kCaptureCap);
        capped = true;
    }

    result.text = QString::fromUtf8(captured);
    result.clipped = capped;
    result.exitCode = process.exitCode();
    if (stopped) {
        result.state = ArgvState::Stopped;
        result.detail = QStringLiteral("已取消");
        return result;
    }
    if (timedOut) {
        result.state = ArgvState::TimedOut;
        result.detail = QStringLiteral("超过 %1 秒仍未结束").arg(task.timeoutMs / 1000);
        return result;
    }
    result.state = ArgvState::Finished;
    return result;
}

QString joinArgvForDisplay(const QString &program, const QStringList &arguments)
{
    QStringList parts;
    parts.reserve(arguments.size() + 1);
    parts << quoteToken(program);
    for (const QString &argument : arguments)
        parts << quoteToken(argument);
    return parts.join(QLatin1Char(' '));
}

} // namespace tw
