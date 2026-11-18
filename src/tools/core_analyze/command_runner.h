#pragma once

#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

namespace tw {

// 一次外部命令。program 必须是可执行文件路径，arguments 逐项传递。
// 不经过 shell，因此路径里的空格、引号和分号不会被解释成别的命令。
struct CommandRequest
{
    QString program;
    QStringList arguments;
    QString workDirectory;
    int timeoutMs = 20000;
};

enum class CommandStatus
{
    Ran,
    MissingProgram,
    FailedToStart,
    TimedOut,
    Cancelled
};

struct CommandResult
{
    CommandStatus status = CommandStatus::FailedToStart;
    int exitCode = -1;
    QString output;
    bool truncated = false;
    QString errorDetail;
};

// 同步执行一条命令。界面线程不要直接调用会等待的实现。
class CommandExecutor
{
public:
    virtual ~CommandExecutor() = default;
    virtual CommandResult run(const CommandRequest &request) = 0;
};

class ProcessExecutor : public CommandExecutor
{
public:
    explicit ProcessExecutor(std::atomic<bool> *cancel = nullptr);

    CommandResult run(const CommandRequest &request) override;

private:
    std::atomic<bool> *m_cancel = nullptr;
};

} // namespace tw
