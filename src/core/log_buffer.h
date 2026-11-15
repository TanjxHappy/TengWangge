#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <deque>

namespace tw {

enum class LogLevel { Info, Success, Warning, Error };

struct LogEntry
{
    QDateTime time;
    LogLevel level = LogLevel::Info;
    QString toolId; // 空串表示平台自身
    QString message;
};

// 平台统一日志环形缓冲。所有工具写入同一份缓冲，每条带来源工具，
// 外壳按当前工具过滤显示。只在 GUI 线程使用。
class LogBuffer : public QObject
{
    Q_OBJECT

public:
    explicit LogBuffer(int capacity = 2000, QObject *parent = nullptr);

    void append(LogLevel level, const QString &toolId, const QString &message);
    void clear();

    int size() const { return static_cast<int>(m_entries.size()); }
    int capacity() const { return m_capacity; }

    // toolId 为空返回全部；否则返回该工具的条目，includePlatform 时并入平台条目。
    QVector<LogEntry> entries(const QString &toolId = QString(), bool includePlatform = false) const;

    static QString levelName(LogLevel level);
    static QString format(const LogEntry &entry);

signals:
    void entryAppended(const tw::LogEntry &entry);
    void cleared();

private:
    std::deque<LogEntry> m_entries;
    int m_capacity;
};

} // namespace tw

Q_DECLARE_METATYPE(tw::LogEntry)
