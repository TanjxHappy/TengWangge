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
#include <cerrno>
#include <utility>
#include <vector>

namespace tw {
namespace {

constexpr int kProgressStride = 64;

QString ioError(int err)
{
    switch (err) {
    case EACCES:
    case EPERM:
        return QStringLiteral("没有权限");
    case ENOENT:
        return QStringLiteral("找不到");
    case ENOTDIR:
        return QStringLiteral("路径中有一项不是目录");
    case ELOOP:
        return QStringLiteral("符号链接层数过多");
    case ENAMETOOLONG:
        return QStringLiteral("路径过长");
    case EROFS:
        return QStringLiteral("目录是只读的");
    case EBUSY:
        return QStringLiteral("文件正在使用");
    case EIO:
        return QStringLiteral("读写出错");
    case ENOSPC:
        return QStringLiteral("磁盘空间不足");
    case EMFILE:
    case ENFILE:
        return QStringLiteral("同时打开的项目过多");
    case EISDIR:
        return QStringLiteral("这是一个目录");
    case EINVAL:
        return QStringLiteral("路径无效");
    default:
        return QStringLiteral("系统未能完成（错误码 %1）").arg(err);
    }
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

// 复制目录描述符后再交给 fdopendir，原描述符继续用于 openat / fstatat。
QStringList listDirectoryNames(int fd, QString *error)
{
    error->clear();
    const int copy = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) {
        *error = QStringLiteral("无法读取目录：%1").arg(ioError(errno));
        return {};
    }
    DIR *raw = ::fdopendir(copy);
    if (!raw) {
        const int err = errno;
        ::close(copy);
        *error = QStringLiteral("无法读取目录：%1").arg(ioError(err));
        return {};
    }
    DirectoryHandle handle(raw);
    QStringList names;
    while (true) {
        errno = 0;
        dirent *entry = ::readdir(handle.get());
        if (!entry) {
            if (errno != 0) {
                *error = QStringLiteral("无法读取目录：%1").arg(ioError(errno));
                names.clear();
            }
            break;
        }
        const QString name = QFile::decodeName(entry->d_name);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;
        names.append(name);
    }
    std::sort(names.begin(), names.end(), [](const QString &left, const QString &right) {
        return QString::compare(left, right, Qt::CaseSensitive) < 0;
    });
    return names;
}

QString unlinkInside(int rootFd, const QString &rootClean, const QString &absolutePath)
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

    if (rootFd < 0)
        return QStringLiteral("无法打开目录：没有权限");

    int current = rootFd;
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
                return QStringLiteral("路径中间有一项不是目录，已拒绝删除");
            return QStringLiteral("删除失败：%1").arg(ioError(err));
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
        return QStringLiteral("无法读取：%1").arg(ioError(err));
    }
    if (S_ISDIR(st.st_mode))
        return QStringLiteral("拒绝删除目录");
    if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode))
        return QStringLiteral("这不是普通文件，已拒绝删除");
    if (::unlinkat(current, base.constData(), 0) != 0) {
        const int err = errno;
        if (err == EISDIR)
            return QStringLiteral("拒绝删除目录");
        if (err == EACCES || err == EPERM)
            return QStringLiteral("删除失败：没有权限");
        if (err == EROFS)
            return QStringLiteral("删除失败：目录是只读的");
        return QStringLiteral("删除失败：%1").arg(ioError(err));
    }
    return {};
}

QStringList matchAgainst(const QStringList &keywords, const QStringList &folded,
                         const QString &relativePath, const QString &fileName)
{
    const QString path = QDir::fromNativeSeparators(relativePath).toCaseFolded();
    const QString name = fileName.toCaseFolded();
    QStringList hits;
    const int count = qMin(keywords.size(), folded.size());
    for (int i = 0; i < count; ++i) {
        const QString &needle = folded.at(i);
        if (needle.isEmpty())
            continue;
        if (path.contains(needle) || name.contains(needle))
            hits.append(keywords.at(i));
    }
    return hits;
}

struct ScanFrame
{
    QString absolutePath;
    QString relativePath;
    FileDescriptor fd;
    QStringList names;
    QStringList subdirs;
    int phase = 0;
    int subdirIndex = 0;
};

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
    QStringList folded;
    folded.reserve(keywords.size());
    for (const QString &keyword : keywords)
        folded.append(keyword.toCaseFolded());
    return matchAgainst(keywords, folded, relativePath, fileName);
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

    // 只跟随用户选中的这一层。子目录一律 O_NOFOLLOW，避免扫描过程中被换成外部链接。
    FileDescriptor rootFd(::open(QFile::encodeName(clean).constData(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!rootFd)
        return fail(QStringLiteral("无法读取该目录：%1").arg(ioError(errno)));

    struct stat rootStat;
    if (::fstat(rootFd.get(), &rootStat) != 0)
        return fail(QStringLiteral("无法读取该目录：%1").arg(ioError(errno)));
    if (!S_ISDIR(rootStat.st_mode))
        return fail(QStringLiteral("路径不是目录：%1").arg(clean));

    QSet<QString> visited;
    visited.insert(visitKey(rootStat));

    QStringList foldedKeywords;
    foldedKeywords.reserve(keywords.size());
    for (const QString &keyword : keywords)
        foldedKeywords.append(keyword.toCaseFolded());

    auto addProblem = [&result, &progress](const QString &path, const QString &reason) {
        result.problems.append(FilterProblem{path, reason});
        const int count = result.problems.size();
        if (count == 1 || count % kProgressStride == 0)
            notify(progress, result.files.size(), count);
    };
    auto addFile = [&](const QString &absolutePath, const QString &relativePath, const QString &fileName) {
        FilterFileRow row;
        row.absolutePath = absolutePath;
        row.relativePath = relativePath;
        row.fileName = fileName;
        row.matchedTerms = matchAgainst(keywords, foldedKeywords, relativePath, fileName);
        row.matched = !row.matchedTerms.isEmpty();
        result.files.append(row);
        const int count = result.files.size();
        if (count == 1 || count % kProgressStride == 0)
            notify(progress, count, result.problems.size());
    };

    QString listError;
    QStringList rootNames = listDirectoryNames(rootFd.get(), &listError);
    if (!listError.isEmpty())
        return fail(listError);

    ScanFrame rootFrame;
    rootFrame.absolutePath = clean;
    rootFrame.fd = std::move(rootFd);
    rootFrame.names = std::move(rootNames);
    std::vector<ScanFrame> stack;
    stack.push_back(std::move(rootFrame));

    while (!stack.empty()) {
        if (cancel.load()) {
            result.cancelled = true;
            result.files.clear();
            return result;
        }

        ScanFrame &frame = stack.back();
        if (frame.phase == 0) {
            // 先分完本层，再进入子目录。这样指向尚未扫描的同级目录的链接不会被误当成循环。
            for (const QString &name : frame.names) {
                if (cancel.load()) {
                    result.cancelled = true;
                    result.files.clear();
                    return result;
                }
                const QByteArray encoded = QFile::encodeName(name);
                const QString childAbsolute = frame.absolutePath + QLatin1Char('/') + name;
                const QString childRelative = frame.relativePath.isEmpty()
                    ? name
                    : frame.relativePath + QLatin1Char('/') + name;
                struct stat st;
                if (::fstatat(frame.fd.get(), encoded.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
                    addProblem(childAbsolute, QStringLiteral("无法读取这一项：%1").arg(ioError(errno)));
                    continue;
                }

                if (S_ISLNK(st.st_mode)) {
                    struct stat target;
                    if (::fstatat(frame.fd.get(), encoded.constData(), &target, 0) != 0) {
                        const int err = errno;
                        if (err == ENOENT)
                            addProblem(childAbsolute, QStringLiteral("符号链接已损坏，目标不存在"));
                        else if (err == ELOOP)
                            addProblem(childAbsolute, QStringLiteral("符号链接形成循环，已跳过"));
                        else
                            addProblem(childAbsolute,
                                       QStringLiteral("无法读取符号链接目标：%1").arg(ioError(err)));
                        continue;
                    }
                    if (S_ISDIR(target.st_mode)) {
                        const QString canonical = QFileInfo(childAbsolute).canonicalFilePath();
                        if (canonical.isEmpty() || !canonicalInside(canonical, rootCanonical))
                            addProblem(childAbsolute, QStringLiteral("符号链接指向起始目录之外，已跳过"));
                        else if (visited.contains(visitKey(target)))
                            addProblem(childAbsolute, QStringLiteral("符号链接形成循环，已跳过"));
                        else
                            addProblem(childAbsolute, QStringLiteral("目录符号链接指向目录内部，未跟随"));
                        continue;
                    }
                    if (S_ISREG(target.st_mode)) {
                        addFile(childAbsolute, childRelative, name);
                        continue;
                    }
                    addProblem(childAbsolute, QStringLiteral("符号链接目标不是普通文件，已跳过"));
                    continue;
                }

                if (S_ISDIR(st.st_mode)) {
                    if (visited.contains(visitKey(st)))
                        addProblem(childAbsolute, QStringLiteral("检测到目录循环，已跳过"));
                    else
                        frame.subdirs.append(name);
                    continue;
                }

                if (S_ISREG(st.st_mode)) {
                    addFile(childAbsolute, childRelative, name);
                    continue;
                }
                addProblem(childAbsolute, QStringLiteral("不是普通文件，已跳过"));
            }
            frame.phase = 1;
            continue;
        }

        if (frame.subdirIndex >= frame.subdirs.size()) {
            stack.pop_back();
            continue;
        }

        const QString subName = frame.subdirs.at(frame.subdirIndex++);
        const int parentFd = frame.fd.get();
        const QString parentAbsolute = frame.absolutePath;
        const QString parentRelative = frame.relativePath;
        const QString childAbsolute = parentAbsolute + QLatin1Char('/') + subName;
        const QString childRelative = parentRelative.isEmpty()
            ? subName
            : parentRelative + QLatin1Char('/') + subName;
        const QByteArray encoded = QFile::encodeName(subName);

        FileDescriptor childFd(::openat(parentFd, encoded.constData(),
                                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!childFd) {
            const int err = errno;
            struct stat now;
            if (::fstatat(parentFd, encoded.constData(), &now, AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK(now.st_mode)) {
                addProblem(childAbsolute, QStringLiteral("这项在扫描时变成了符号链接，已跳过"));
            } else {
                addProblem(childAbsolute, QStringLiteral("无法读取目录：%1").arg(ioError(err)));
            }
            continue;
        }

        struct stat opened;
        if (::fstat(childFd.get(), &opened) != 0) {
            addProblem(childAbsolute, QStringLiteral("无法读取目录：%1").arg(ioError(errno)));
            continue;
        }
        if (!S_ISDIR(opened.st_mode)) {
            addProblem(childAbsolute, QStringLiteral("无法读取目录：这不是目录"));
            continue;
        }
        if (visited.contains(visitKey(opened))) {
            addProblem(childAbsolute, QStringLiteral("检测到目录循环，已跳过"));
            continue;
        }
        const QString canonical = QFileInfo(childAbsolute).canonicalFilePath();
        if (!canonicalInside(canonical, rootCanonical)) {
            addProblem(childAbsolute, QStringLiteral("目录超出起始范围，已跳过"));
            continue;
        }

        QString childListError;
        QStringList childNames = listDirectoryNames(childFd.get(), &childListError);
        if (!childListError.isEmpty()) {
            addProblem(childAbsolute, childListError);
            continue;
        }

        visited.insert(visitKey(opened));
        ScanFrame child;
        child.absolutePath = childAbsolute;
        child.relativePath = childRelative;
        child.fd = std::move(childFd);
        child.names = std::move(childNames);
        stack.push_back(std::move(child));
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
    if (rootClean.isEmpty() || rootInfo.isRelative() || !rootInfo.isDir()
        || rootInfo.canonicalFilePath().isEmpty()) {
        result.error = QStringLiteral("所选目录无效，已拒绝删除，没有改动任何文件");
        return result;
    }
    if (rootClean == QLatin1String("/") || rootInfo.canonicalFilePath() == QLatin1String("/")) {
        result.error = QStringLiteral("不能在文件系统根目录 / 上删除");
        return result;
    }

    FileDescriptor rootFd(::open(QFile::encodeName(rootClean).constData(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!rootFd) {
        result.error = QStringLiteral("无法打开所选目录，已拒绝删除：%1").arg(ioError(errno));
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
        item.error = unlinkInside(rootFd.get(), rootClean, item.absolutePath);
        item.removed = item.error.isEmpty();
        result.items.append(item);
        if (i == 0 || (i + 1) % kProgressStride == 0)
            notify(progress, i + 1, total);
    }
    notify(progress, total, total);
    return result;
}

} // namespace tw
