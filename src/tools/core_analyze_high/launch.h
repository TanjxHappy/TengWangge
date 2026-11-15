#pragma once

#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

namespace tw {

// 一次外部程序调用。program 是可执行文件路径，arguments 逐项传递。
// 调用方不得把整条命令拼成一个字符串交给 shell。
struct ArgvTask
{
    QString program;
    QStringList arguments;
    QString directory;
    int timeoutMs = 20000;
};

enum class ArgvState
{
    Finished,
    NotFound,
    StartFailed,
    TimedOut,
    Stopped
};

struct ArgvResult
{
    ArgvState state = ArgvState::StartFailed;
    int exitCode = -1;
    QString text;
    bool clipped = false;
    QString detail;
};

class ArgvHost
{
public:
    ArgvHost() = default;
    virtual ~ArgvHost() = default;

    ArgvHost(const ArgvHost &) = delete;
    ArgvHost &operator=(const ArgvHost &) = delete;

    virtual ArgvResult run(const ArgvTask &task) = 0;
};

// 用 QProcess 直接启动程序。不调用 shell，因此分号、引号和 $() 都只是参数文本。
class SystemArgvHost : public ArgvHost
{
public:
    explicit SystemArgvHost(std::atomic<bool> *stop = nullptr);

    ArgvResult run(const ArgvTask &task) override;

private:
    std::atomic<bool> *m_stop = nullptr;
};

// 仅用于把已经拆开的参数显示成一行，不参与真正启动。
QString joinArgvForDisplay(const QString &program, const QStringList &arguments);

} // namespace tw
