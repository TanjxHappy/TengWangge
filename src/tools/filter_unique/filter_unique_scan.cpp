#include "tools/filter_unique/filter_unique_scan.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dirent.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace tw {
namespace {

QString errnoText(int err)
{
    return QString::fromStdString(std::generic_category().message(err));
}

QString visitKey(const struct stat &st)
{
    return QString::number(static_cast<quint64>(st.st_dev)) + QLatin1Char(':')
        + QString::number(static_cast<quint64>(st.st_ino));
}

bool canonicalInside(const QString &canonical, const QString &rootCanonical)
{
    if (canonical.isEmpty() || rootCanonical.isEmpty())
        return false;
    if (canonical == rootCanonical)
        return true;
    const QString prefix = rootCanonical.endsWith(QLatin1Char('/'))
        ? rootCanonical
        : rootCanonical + QLatin1Char('/');
    return canonical.startsWith(prefix);
}

void notify(const std::function<void(int, int)> &progress, int first, int second)
{
    if (progress)
        progress(first, second);
}

class FileDescriptor
{
public:
    explicit FileDescriptor(int fd = -1)
        : m_fd(fd)
    {
    }

    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    FileDescriptor(FileDescriptor &&other) noexcept
        : m_fd(other.m_fd)
    {
        other.m_fd = -1;
    }

    FileDescriptor &operator=(FileDescriptor &&other) noexcept
    {
        if (this != &other) {
            reset();
            m_fd = other.m_fd;
            other.m_fd = -1;
        }
        return *this;
    }

    int get() const { return m_fd; }
    explicit operator bool() const { return m_fd >= 0; }

    void reset(int fd = -1)
    {
        if (m_fd >= 0)
            ::close(m_fd);
        m_fd = fd;
    }

private:
    int m_fd = -1;
};

class DirectoryHandle
{
public:
    explicit DirectoryHandle(DIR *dir = nullptr)
        : m_dir(dir)
    {
    }

    ~DirectoryHandle()
    {
        if (m_dir)
            ::closedir(m_dir);
    }

    DirectoryHandle(const DirectoryHandle &) = delete;
    DirectoryHandle &operator=(const DirectoryHandle &) = delete;

    explicit operator bool() const { return m_dir != nullptr; }
    DIR *get() const { return m_dir; }

private:
    DIR *m_dir = nullptr;
};

QString unlinkInside(const QString &rootClean, const QString &absolutePath)
{
    const QString clean = QDir::cleanPath(absolutePath.trimmed());
    if (clean.isEmpty() || clean == rootClean)
        return QStringLiteral("拒绝删除目录");

    const QString prefix = rootClean + QLatin1Char('/');
    if (!clean.startsWith(prefix))
        return QStringLiteral("路径不在所选目录内");

    const QString relative = QDir::fromNativeSeparators(QDir(rootClean).relativeFilePath(clean));
    if (relative.isEmpty() || relative == QLatin1String(".") || relative == QLatin1String("..")
        || relative.startsWith(QLatin1String("../")) || relative.contains(QLatin1String("/../"))) {
        return QStringLiteral("路径不在所选目录内");
    }

    const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return QStringLiteral("拒绝删除目录");
    for (const QString &part : parts) {
        if (part == QLatin1String(".") || part == QLatin1String(".."))
            return QStringLiteral("路径不在所选目录内");
    }

    FileDescriptor rootFd(::open(QFile::encodeName(rootClean).constData(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!rootFd)
        return QStringLiteral("无法打开目录：%1").arg(errnoText(errno));

    int current = rootFd.get();
    FileDescriptor nested;
    for (int i = 0; i < parts.size() - 1; ++i) {
        const QByteArray name = QFile::encodeName(parts.at(i));
        const int next = ::openat(current, name.constData(),
                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            const int err = errno;
            struct stat linkStat;
            if (::fstatat(current, name.constData(), &linkStat, AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK(linkStat.st_mode)) {
                return QStringLiteral("路径经过符号链接，已拒绝");
            }
            if (err == ELOOP || err == EMLINK)
                return QStringLiteral("路径经过符号链接，已拒绝");
            if (err == ENOENT)
                return QStringLiteral("文件已不存在");
            if (err == ENOTDIR)
                return QStringLiteral("路径中间项不是目录，已拒绝");
            return QStringLiteral("删除失败：%1").arg(errnoText(err));
        }
        nested.reset(next);
        current = next;
    }

    const QByteArray base = QFile::encodeName(parts.last());
    struct stat st;
    if (::fstatat(current, base.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        const int err = errno;
        if (err == ENOENT)
            return QStringLiteral("文件已不存在");
        return QStringLiteral("无法读取：%1").arg(errnoText(err));
    }
    if (S_ISDIR(st.st_mode))
        return QStringLiteral("拒绝删除目录");
    if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode))
        return QStringLiteral("拒绝删除该类型的文件");
    if (::unlinkat(current, base.constData(), 0) != 0)
        return QStringLiteral("删除失败：%1").arg(errnoText(errno));
    return {};
}

} // namespace

QStringList parseKeywords(const QString &text)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QStringList keywords;
    QSet<QString> seen;
    const QStringList lines = normalized.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;
        const QString folded = trimmed.toCaseFolded();
        if (seen.contains(folded))
            continue;
        seen.insert(folded);
        keywords.append(trimmed);
    }
    return keywords;
}

QStringList matchedKeywords(const QStringList &keywords, const QString &relativePath,
                            const QString &fileName)
{
    const QString path = QDir::fromNativeSeparators(relativePath).toCaseFolded();
    const QString name = fileName.toCaseFolded();
    QStringList hits;
    for (const QString &keyword : keywords) {
        const QString folded = keyword.toCaseFolded();
        if (folded.isEmpty())
            continue;
        if (path.contains(folded) || name.contains(folded))
            hits.append(keyword);
    }
    return hits;
}

FilterScanResult scanDirectory(const QString &rootPath, const QStringList &keywords,
                               const std::atomic<bool> &cancel,
                               const std::function<void(int files, int problems)> &progress)
{
    FilterScanResult result;
    const QString clean = QDir::cleanPath(rootPath.trimmed());
    result.rootPath = clean;

    auto fail = [&result](const QString &message) {
        result.error = message;
        return result;
    };

    if (cancel.load()) {
        result.cancelled = true;
        return result;
    }
    if (clean.isEmpty() || QFileInfo(clean).isRelative())
        return fail(QStringLiteral("目录必须是已存在的绝对路径"));
    if (clean == QLatin1String("/"))
        return fail(QStringLiteral("不能选择文件系统根目录"));

    const QFileInfo rootInfo(clean);
    if (!rootInfo.exists())
        return fail(QStringLiteral("目录不存在：%1").arg(clean));
    if (!rootInfo.isDir())
        return fail(QStringLiteral("路径不是目录：%1").arg(clean));

    const QString rootCanonical = rootInfo.canonicalFilePath();
    if (rootCanonical.isEmpty())
        return fail(QStringLiteral("无法读取该目录：%1").arg(clean));
    if (rootCanonical == QLatin1String("/"))
        return fail(QStringLiteral("不能选择文件系统根目录"));

    struct stat rootStat;
    if (::stat(QFile::encodeName(clean).constData(), &rootStat) != 0)
        return fail(QStringLiteral("无法读取该目录：%1").arg(errnoText(errno)));
    if (!S_ISDIR(rootStat.st_mode))
        return fail(QStringLiteral("路径不是目录：%1").arg(clean));

    QSet<QString> visited;
    visited.insert(visitKey(rootStat));

    QVector<QString> directories;
    directories.append(clean);

    auto addProblem = [&result](const QString &path, const QString &reason) {
        result.problems.append(FilterProblem{path, reason});
    };
    auto addFile = [&](const QString &absolutePath) {
        FilterFileRow row;
        row.absolutePath = absolutePath;
        row.relativePath = QDir::fromNativeSeparators(QDir(clean).relativeFilePath(absolutePath));
        row.fileName = QFileInfo(absolutePath).fileName();
        row.matchedTerms = matchedKeywords(keywords, row.relativePath, row.fileName);
        row.matched = !row.matchedTerms.isEmpty();
        result.files.append(row);
        const int count = result.files.size();
        if (count == 1 || count % 32 == 0)
            notify(progress, count, result.problems.size());
    };

    for (int index = 0; index < directories.size(); ++index) {
        if (cancel.load()) {
            result.cancelled = true;
            result.files.clear();
            return result;
        }

        const QString dirPath = directories.at(index);
        DirectoryHandle handle(::opendir(QFile::encodeName(dirPath).constData()));
        if (!handle) {
            const QString reason = QStringLiteral("无法读取目录：%1").arg(errnoText(errno));
            addProblem(dirPath, reason);
            if (index == 0) {
                result.error = reason;
                return result;
            }
            continue;
        }

        QStringList names;
        while (dirent *entry = ::readdir(handle.get())) {
            const QString name = QFile::decodeName(entry->d_name);
            if (name == QLatin1String(".") || name == QLatin1String(".."))
                continue;
            names.append(name);
        }
        std::sort(names.begin(), names.end(), [](const QString &left, const QString &right) {
            return QString::compare(left, right, Qt::CaseSensitive) < 0;
        });

        for (const QString &name : names) {
            if (cancel.load()) {
                result.cancelled = true;
                result.files.clear();
                return result;
            }

            const QString childPath = QDir::cleanPath(QDir(dirPath).filePath(name));
            struct stat st;
            if (::lstat(QFile::encodeName(childPath).constData(), &st) != 0) {
                addProblem(childPath, QStringLiteral("无法读取：%1").arg(errnoText(errno)));
                continue;
            }

            if (S_ISLNK(st.st_mode)) {
                struct stat target;
                if (::stat(QFile::encodeName(childPath).constData(), &target) != 0) {
                    addProblem(childPath, QStringLiteral("无法读取符号链接目标"));
                    continue;
                }
                const QString canonical = QFileInfo(childPath).canonicalFilePath();
                if (S_ISDIR(target.st_mode)) {
                    if (canonical.isEmpty() || !canonicalInside(canonical, rootCanonical))
                        addProblem(childPath, QStringLiteral("符号链接指向起始目录之外，已跳过"));
                    else if (visited.contains(visitKey(target)))
                        addProblem(childPath, QStringLiteral("符号链接形成循环，已跳过"));
                    else
                        addProblem(childPath, QStringLiteral("目录符号链接指向目录内部，未跟随"));
                    continue;
                }
                if (S_ISREG(target.st_mode)) {
                    addFile(childPath);
                    continue;
                }
                addProblem(childPath, QStringLiteral("符号链接目标不是普通文件，已跳过"));
                continue;
            }

            if (S_ISDIR(st.st_mode)) {
                const QString key = visitKey(st);
                if (visited.contains(key)) {
                    addProblem(childPath, QStringLiteral("检测到目录循环，已跳过"));
                    continue;
                }
                const QString canonical = QFileInfo(childPath).canonicalFilePath();
                if (!canonicalInside(canonical, rootCanonical)) {
                    addProblem(childPath, QStringLiteral("目录超出起始范围，已跳过"));
                    continue;
                }
                visited.insert(key);
                directories.append(childPath);
                continue;
            }

            if (S_ISREG(st.st_mode)) {
                addFile(childPath);
                continue;
            }
            addProblem(childPath, QStringLiteral("不是普通文件，已跳过"));
        }
    }

    std::sort(result.files.begin(), result.files.end(),
              [](const FilterFileRow &left, const FilterFileRow &right) {
                  const int byPath = QString::compare(left.relativePath, right.relativePath,
                                                      Qt::CaseSensitive);
                  if (byPath != 0)
                      return byPath < 0;
                  return QString::compare(left.absolutePath, right.absolutePath, Qt::CaseSensitive) < 0;
              });
    notify(progress, result.files.size(), result.problems.size());
    return result;
}

FilterDeleteResult deleteListedFiles(const QString &rootPath, const QStringList &absolutePaths,
                                     const std::atomic<bool> &cancel,
                                     const std::function<void(int done, int total)> &progress)
{
    FilterDeleteResult result;
    const QString rootClean = QDir::cleanPath(rootPath.trimmed());
    const QFileInfo rootInfo(rootClean);
    if (rootClean.isEmpty() || rootInfo.isRelative() || rootClean == QLatin1String("/")
        || rootInfo.canonicalFilePath().isEmpty() || rootInfo.canonicalFilePath() == QLatin1String("/")
        || !rootInfo.isDir()) {
        result.error = QStringLiteral("目录无效，已拒绝删除");
        return result;
    }

    const int total = absolutePaths.size();
    for (int i = 0; i < total; ++i) {
        if (cancel.load()) {
            result.cancelled = true;
            notify(progress, i, total);
            return result;
        }
        FilterDeleteItem item;
        item.absolutePath = QDir::cleanPath(absolutePaths.at(i));
        item.error = unlinkInside(rootClean, item.absolutePath);
        item.removed = item.error.isEmpty();
        result.items.append(item);
        if (i == 0 || (i + 1) % 32 == 0)
            notify(progress, i + 1, total);
    }
    notify(progress, total, total);
    return result;
}

} // namespace tw
