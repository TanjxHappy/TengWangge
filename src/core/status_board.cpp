#include "core/status_board.h"

#include <QTimer>

namespace tw {

StatusBoard::StatusBoard(QObject *parent)
    : QObject(parent)
{
}

int StatusBoard::defaultTimeout(StatusState state)
{
    switch (state) {
    case StatusState::Info:
    case StatusState::Success:
        return 6000;
    case StatusState::Idle:
    case StatusState::Busy:
    case StatusState::Error:
        return 0;
    }
    return 0;
}

QString StatusBoard::idleText()
{
    return QStringLiteral("就绪");
}

QString StatusBoard::stateName(StatusState state)
{
    switch (state) {
    case StatusState::Idle: return QStringLiteral("idle");
    case StatusState::Info: return QStringLiteral("info");
    case StatusState::Busy: return QStringLiteral("busy");
    case StatusState::Success: return QStringLiteral("success");
    case StatusState::Error: return QStringLiteral("error");
    }
    return QStringLiteral("idle");
}

void StatusBoard::set(const QString &toolId, StatusState state, const QString &text, int timeoutMs)
{
    const bool blank = text.trimmed().isEmpty();
    const StatusState effective = blank ? StatusState::Idle : state;
    const quint64 generation = m_nextGeneration++;
    Slot &slot = m_slots[toolId];
    slot.message = StatusMessage{effective, blank ? idleText() : text};
    slot.generation = generation;
    emit changed(toolId);

    const int timeout = timeoutMs == UseDefaultTimeout ? defaultTimeout(effective) : timeoutMs;
    if (timeout <= 0 || effective == StatusState::Idle)
        return;
    QTimer::singleShot(timeout, this, [this, toolId, generation]() {
        const auto it = m_slots.constFind(toolId);
        if (it != m_slots.constEnd() && it->generation == generation)
            clear(toolId);
    });
}

void StatusBoard::clear(const QString &toolId)
{
    Slot &slot = m_slots[toolId];
    slot.message = StatusMessage{StatusState::Idle, idleText()};
    slot.generation = m_nextGeneration++;
    emit changed(toolId);
}

StatusMessage StatusBoard::current(const QString &toolId) const
{
    const auto it = m_slots.constFind(toolId);
    if (it == m_slots.constEnd())
        return StatusMessage{StatusState::Idle, idleText()};
    return it->message;
}

} // namespace tw
