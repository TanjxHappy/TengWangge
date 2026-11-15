#include "core/log_buffer.h"

#include <algorithm>

namespace tw {

LogBuffer::LogBuffer(int capacity, QObject *parent)
    : QObject(parent)
    , m_capacity(std::max(1, capacity))
{
}

void LogBuffer::append(LogLevel level, const QString &toolId, const QString &message)
{
    LogEntry entry;
    entry.time = QDateTime::currentDateTime();
    entry.level = level;
    entry.toolId = toolId;
    entry.message = message;
    m_entries.push_back(entry);
    while (static_cast<int>(m_entries.size()) > m_capacity)
        m_entries.pop_front();
    emit entryAppended(entry);
}

void LogBuffer::clear()
{
    m_entries.clear();
    emit cleared();
}

QVector<LogEntry> LogBuffer::entries(const QString &toolId, bool includePlatform) const
{
    QVector<LogEntry> result;
    for (const LogEntry &entry : m_entries) {
        if (toolId.isEmpty() || entry.toolId == toolId
            || (includePlatform && entry.toolId.isEmpty())) {
            result.append(entry);
        }
    }
    return result;
}

QString LogBuffer::levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Info: return QStringLiteral("信息");
    case LogLevel::Success: return QStringLiteral("成功");
    case LogLevel::Warning: return QStringLiteral("警告");
    case LogLevel::Error: return QStringLiteral("错误");
    }
    return QStringLiteral("信息");
}

QString LogBuffer::format(const LogEntry &entry)
{
    return QStringLiteral("%1  [%2]  %3")
        .arg(entry.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
             levelName(entry.level), entry.message);
}

} // namespace tw
