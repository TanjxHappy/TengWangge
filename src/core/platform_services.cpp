#include "core/platform_services.h"

namespace tw {

PlatformServices::PlatformServices(const DataPaths &paths, QObject *parent)
    : QObject(parent)
    , m_paths(paths)
    , m_log(2000)
{
}

ToolServices PlatformServices::servicesFor(const QString &toolId)
{
    return ToolServices(this, toolId);
}

ToolServices::ToolServices(PlatformServices *platform, const QString &toolId)
    : m_platform(platform)
    , m_toolId(toolId)
{
}

void ToolServices::log(LogLevel level, const QString &message) const
{
    if (m_platform)
        m_platform->log()->append(level, m_toolId, message);
}

void ToolServices::showStatus(StatusState state, const QString &text, int timeoutMs) const
{
    if (m_platform)
        m_platform->status()->set(m_toolId, state, text, timeoutMs);
}

void ToolServices::clearStatus() const
{
    if (m_platform)
        m_platform->status()->clear(m_toolId);
}

NavigationLock ToolServices::lockNavigation(const QString &reason) const
{
    if (!m_platform)
        return NavigationLock();
    return NavigationLock(m_platform->gate(), m_toolId, reason);
}

QString ToolServices::dataDir(QString *error) const
{
    if (!m_platform) {
        if (error)
            *error = QStringLiteral("平台服务已不可用");
        return {};
    }
    return m_platform->paths().toolDataDir(m_toolId, error);
}

} // namespace tw
