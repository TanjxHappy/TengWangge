#include "core/data_paths.h"

#include "core/app_info.h"
#include "core/tool_info.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QStringList>

namespace tw {
namespace {

QString absoluteClean(const QString &path)
{
    if (path.trimmed().isEmpty())
        return {};
    return QDir::cleanPath(QDir::current().absoluteFilePath(path.trimmed()));
}

bool ensureDirectory(const QString &path, QString *error)
{
    if (QDir(path).exists())
        return true;
    if (QDir().mkpath(path))
        return true;
    if (error)
        *error = QStringLiteral("无法创建数据目录：%1").arg(path);
    return false;
}

} // namespace

const char *DataPaths::overrideVariable()
{
    return "TENGWANGGE_DATA_DIR";
}

DataPaths::DataPaths(const QString &root)
    : m_root(absoluteClean(root))
{
}

DataPaths DataPaths::standard()
{
    const QString configured = qEnvironmentVariable(overrideVariable());
    if (!configured.trimmed().isEmpty())
        return DataPaths(configured);

    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        base = QDir::home().filePath(QStringLiteral(".local/share"));
    return DataPaths(QDir(base).filePath(AppInfo::dataDirName()));
}

QString DataPaths::logDir() const
{
    return QDir(m_root).filePath(QStringLiteral("logs"));
}

QString DataPaths::rootProblem(const QString &root)
{
    const QString clean = absoluteClean(root);
    if (clean.isEmpty())
        return QStringLiteral("数据根目录为空");
    if (clean == QLatin1String("/"))
        return QStringLiteral("数据根目录不能是文件系统根目录");

    static const QStringList systemRoots{
        QStringLiteral("/bin"),  QStringLiteral("/boot"), QStringLiteral("/dev"),
        QStringLiteral("/etc"),  QStringLiteral("/lib"),  QStringLiteral("/lib32"),
        QStringLiteral("/lib64"), QStringLiteral("/proc"), QStringLiteral("/sbin"),
        QStringLiteral("/sys"),  QStringLiteral("/usr"),
    };
    for (const QString &system : systemRoots) {
        if (clean == system || clean.startsWith(system + QLatin1Char('/')))
            return QStringLiteral("数据根目录位于系统目录下，已拒绝：%1").arg(clean);
    }
    return {};
}

QString DataPaths::toolDataDir(const QString &toolId, QString *error) const
{
    if (!isValidIdentifier(toolId)) {
        if (error)
            *error = QStringLiteral("工具标识无效：%1").arg(toolId);
        return {};
    }
    const QString problem = rootProblem(m_root);
    if (!problem.isEmpty()) {
        if (error)
            *error = problem;
        return {};
    }

    const bool rootExisted = QDir(m_root).exists();
    if (!ensureDirectory(m_root, error))
        return {};
    if (!rootExisted) {
        QFile::setPermissions(m_root, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                          | QFileDevice::ExeOwner);
    }

    const QString path = QDir(m_root).filePath(QStringLiteral("tools/") + toolId);
    if (!ensureDirectory(path, error))
        return {};
    return path;
}

} // namespace tw
