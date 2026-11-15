#include "tools/filter_unique_high/bounded_tree.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tw {
namespace {

QString errorText(int err)
{
    const std::string text = std::generic_category().message(err);
    if (text.empty())
        return QStringLiteral("错误码 %1").arg(err);
    return QString::fromStdString(text);
}

QString cleanSlashes(const QString &path)
{
    if (!path.startsWith(QLatin1Char('/')))
        return {};
    QString out;
    out.reserve(path.size());
    bool slash = false;
    for (const QChar ch : path) {
        if (ch == QLatin1Char('/')) {
            if (slash)
                continue;
            slash = true;
        } else {
            slash = false;
        }
        out.append(ch);
    }
    if (out.size() > 1 && out.endsWith(QLatin1Char('/')))
        out.chop(1);
    return out;
}

QString extendRelative(const QString &parent, const QString &name)
{
    if (parent.isEmpty())
        return name;
    return parent + QLatin1Char('/') + name;
}

QString childPath(const QString &root, const QString &relative)
{
    if (relative.isEmpty())
        return root;
    return root + QLatin1Char('/') + relative;
}

bool stopRequested(const std::atomic<bool> &cancel)
{
    return cancel.load();
}

// 返回 true 表示调用方应停下来。paceMs 只在还没被取消时睡眠，方便测试插进取消。
bool paused(const std::atomic<bool> &cancel, int paceMs)
{
    if (stopRequested(cancel))
        return true;
    if (paceMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(paceMs));
    return stopRequested(cancel);
}

class OwnedFd
{
public:
    OwnedFd() = default;
    explicit OwnedFd(int fd)
        : m_fd(fd)
    {
    }
    ~OwnedFd() { reset(); }

    OwnedFd(const OwnedFd &) = delete;
    OwnedFd &operator=(const OwnedFd &) = delete;

    OwnedFd(OwnedFd &&other) noexcept
        : m_fd(other.m_fd)
    {
        other.m_fd = -1;
    }

    OwnedFd &operator=(OwnedFd &&other) noexcept
    {
        if (this != &other) {
            reset();
            m_fd = other.m_fd;
            other.m_fd = -1;
        }
        return *this;
    }

    int get() const { return m_fd; }

    void reset()
    {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

private:
    int m_fd = -1;
};

struct NodeId
{
    dev_t device = 0;
    ino_t inode = 0;

    bool operator==(const NodeId &other) const
    {
        return device == other.device && inode == other.inode;
    }
};

bool chainContains(const std::vector<NodeId> &chain, const NodeId &id)
{
    return std::find(chain.begin(), chain.end(), id) != chain.end();
}

// 从 candidate 沿 ".." 往上走，看会不会遇到 rootFd 指向的目录。
// 遇不到就说明这个目录不在起始目录里面（包括通过符号链接跳到外面的情况）。
bool directoryContains(int rootFd, int candidateFd)
{
    struct stat rootStat;
    struct stat currentStat;
    if (::fstat(rootFd, &rootStat) != 0 || ::fstat(candidateFd, &currentStat) != 0)
        return false;

    OwnedFd cursor(::dup(candidateFd));
    if (cursor.get() < 0)
        return false;

    for (int hop = 0; hop < 256; ++hop) {
        if (currentStat.st_dev == rootStat.st_dev && currentStat.st_ino == rootStat.st_ino)
            return true;

        const int parent = ::openat(cursor.get(), "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0)
            return false;
        struct stat parentStat;
        if (::fstat(parent, &parentStat) != 0) {
            ::close(parent);
            return false;
        }
        if (parentStat.st_dev == currentStat.st_dev && parentStat.st_ino == currentStat.st_ino) {
            ::close(parent);
            return false;
        }
        cursor = OwnedFd(parent);
        currentStat = parentStat;
    }
    return false;
}

struct Frame
{
    OwnedFd fd;
    QString relative;
    std::vector<NodeId> ancestors;

    Frame(OwnedFd handle, QString rel, std::vector<NodeId> chain)
        : fd(std::move(handle))
        , relative(std::move(rel))
        , ancestors(std::move(chain))
    {
    }
};

struct WalkContext
{
    const QVector<Term> *terms = nullptr;
    const std::atomic<bool> *cancel = nullptr;
    std::function<void(int, int)> progress;
    QString rootPath;
    int rootFd = -1;
    TreeWalk *report = nullptr;
    std::vector<Frame> *pending = nullptr;
};

void notify(WalkContext &ctx)
{
    if (ctx.progress)
        ctx.progress(ctx.report->files.size(), ctx.report->issues.size());
}

void note(WalkContext &ctx, const QString &path, const QString &reason)
{
    ctx.report->issues.append(TreeIssue{path, reason});
    notify(ctx);
}

void acceptFile(WalkContext &ctx, const QString &relative, const QString &name)
{
    ListedFile file;
    file.absolutePath = childPath(ctx.rootPath, relative);
    file.relativePath = relative;
    file.fileName = name;
    file.hits = termsHitting(*ctx.terms, relative, name);
    file.matched = !file.hits.isEmpty();
    ctx.report->files.append(file);
    notify(ctx);
}

void classifyLink(WalkContext &ctx, const Frame &frame, const QByteArray &raw,
                  const QString &relative, const QString &absolute)
{
    struct stat followed;
    if (::fstatat(frame.fd.get(), raw.constData(), &followed, 0) != 0) {
        note(ctx, absolute, QStringLiteral("无法读取符号链接目标"));
        return;
    }

    if (S_ISDIR(followed.st_mode)) {
        const NodeId target{followed.st_dev, followed.st_ino};
        if (chainContains(frame.ancestors, target)) {
            note(ctx, absolute, QStringLiteral("符号链接形成循环，已跳过"));
            return;
        }
        OwnedFd opened(::openat(frame.fd.get(), raw.constData(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (opened.get() < 0) {
            note(ctx, absolute, QStringLiteral("无法读取符号链接目标"));
            return;
        }
        if (directoryContains(ctx.rootFd, opened.get()))
            note(ctx, absolute, QStringLiteral("目录符号链接指向目录内部，未跟随"));
        else
            note(ctx, absolute, QStringLiteral("符号链接指向起始目录之外，已跳过"));
        return;
    }

    if (S_ISREG(followed.st_mode)) {
        const QString name = relative.section(QLatin1Char('/'), -1);
        acceptFile(ctx, relative, name);
        return;
    }

    note(ctx, absolute, QStringLiteral("不是普通文件，已跳过"));
}

void descend(WalkContext &ctx, const Frame &frame, const QByteArray &raw,
             const QString &relative, const QString &absolute, const NodeId &id)
{
    if (chainContains(frame.ancestors, id)) {
        note(ctx, absolute, QStringLiteral("目录形成循环，已跳过"));
        return;
    }

    const int child = ::openat(frame.fd.get(), raw.constData(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) {
        note(ctx, absolute, QStringLiteral("无法读取目录：%1").arg(errorText(errno)));
        return;
    }

    std::vector<NodeId> chain = frame.ancestors;
    chain.push_back(id);
    ctx.pending->push_back(Frame(OwnedFd(child), relative, std::move(chain)));
}

void readDirectory(WalkContext &ctx, Frame &frame)
{
    const int dupfd = ::dup(frame.fd.get());
    if (dupfd < 0) {
        note(ctx, childPath(ctx.rootPath, frame.relative),
             QStringLiteral("无法读取目录：%1").arg(errorText(errno)));
        return;
    }
    DIR *dir = ::fdopendir(dupfd);
    if (dir == nullptr) {
        const int err = errno;
        ::close(dupfd);
        note(ctx, childPath(ctx.rootPath, frame.relative),
             QStringLiteral("无法读取目录：%1").arg(errorText(err)));
        return;
    }

    std::vector<QByteArray> names;
    while (true) {
        errno = 0;
        dirent *ent = ::readdir(dir);
        if (ent == nullptr) {
            if (errno != 0) {
                note(ctx, childPath(ctx.rootPath, frame.relative),
                     QStringLiteral("无法读取目录：%1").arg(errorText(errno)));
            }
            break;
        }
        const QByteArray raw(ent->d_name);
        if (raw == "." || raw == "..")
            continue;
        names.push_back(raw);
    }
    ::closedir(dir);

    for (const QByteArray &raw : names) {
        if (stopRequested(*ctx.cancel)) {
            ctx.report->cancelled = true;
            return;
        }
        const QString name = QFile::decodeName(raw);
        const QString relative = extendRelative(frame.relative, name);
        const QString absolute = childPath(ctx.rootPath, relative);

        struct stat linkStat;
        if (::fstatat(frame.fd.get(), raw.constData(), &linkStat, AT_SYMLINK_NOFOLLOW) != 0) {
            note(ctx, absolute, QStringLiteral("无法读取：%1").arg(errorText(errno)));
            continue;
        }
        if (S_ISLNK(linkStat.st_mode)) {
            classifyLink(ctx, frame, raw, relative, absolute);
            continue;
        }
        if (S_ISDIR(linkStat.st_mode)) {
            descend(ctx, frame, raw, relative, absolute, NodeId{linkStat.st_dev, linkStat.st_ino});
            continue;
        }
        if (S_ISREG(linkStat.st_mode)) {
            acceptFile(ctx, relative, name);
            continue;
        }
        note(ctx, absolute, QStringLiteral("不是普通文件，已跳过"));
    }
}

QString relativeChild(const QString &root, const QString &absolute, QString *error)
{
    const QString cleanRoot = QDir::cleanPath(root);
    const QString cleanPath = cleanSlashes(absolute);
    if (cleanRoot.isEmpty() || cleanRoot == QLatin1String("/") || cleanPath.isEmpty()) {
        *error = QStringLiteral("路径不在所选目录内");
        return {};
    }
    if (cleanPath == cleanRoot) {
        *error = QStringLiteral("拒绝删除目录");
        return {};
    }
    const QString prefix = cleanRoot + QLatin1Char('/');
    if (!cleanPath.startsWith(prefix)) {
        *error = QStringLiteral("路径不在所选目录内");
        return {};
    }
    const QString relative = cleanPath.mid(prefix.size());
    const QStringList parts = relative.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")) {
            *error = QStringLiteral("路径不在所选目录内");
            return {};
        }
    }
    return relative;
}

QString unlinkInside(const QString &root, const QString &absolute)
{
    QString error;
    const QString relative = relativeChild(root, absolute, &error);
    if (!error.isEmpty())
        return error;

    const QString cleanRoot = QDir::cleanPath(root);
    const int rootFd = ::open(QFile::encodeName(cleanRoot).constData(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootFd < 0)
        return QStringLiteral("无法读取目录：%1").arg(errorText(errno));

    OwnedFd current(rootFd);
    const QStringList parts = relative.split(QLatin1Char('/'));
    for (int index = 0; index < parts.size(); ++index) {
        const bool last = index + 1 == parts.size();
        const QByteArray name = QFile::encodeName(parts.at(index));
        struct stat st;
        if (::fstatat(current.get(), name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            const int err = errno;
            if (err == ENOENT)
                return last ? QStringLiteral("文件不存在") : QStringLiteral("路径不在所选目录内");
            return QStringLiteral("无法读取：%1").arg(errorText(err));
        }

        if (!last) {
            if (S_ISLNK(st.st_mode))
                return QStringLiteral("路径经过符号链接，已拒绝");
            if (!S_ISDIR(st.st_mode))
                return QStringLiteral("路径不在所选目录内");
            const int next = ::openat(current.get(), name.constData(),
                                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0) {
                const int err = errno;
                if (err == ELOOP || err == ENOTDIR)
                    return QStringLiteral("路径经过符号链接，已拒绝");
                return QStringLiteral("无法读取目录：%1").arg(errorText(err));
            }
            current = OwnedFd(next);
            continue;
        }

        if (S_ISDIR(st.st_mode))
            return QStringLiteral("拒绝删除目录");
        if (S_ISLNK(st.st_mode)) {
            struct stat followed;
            if (::fstatat(current.get(), name.constData(), &followed, 0) == 0
                && S_ISDIR(followed.st_mode))
                return QStringLiteral("拒绝删除目录");
        } else if (!S_ISREG(st.st_mode)) {
            return QStringLiteral("不是可删除的普通文件");
        }

        if (::unlinkat(current.get(), name.constData(), 0) != 0) {
            const int err = errno;
            if (err == EISDIR || err == EPERM)
                return QStringLiteral("拒绝删除目录");
            return QStringLiteral("删除失败：%1").arg(errorText(err));
        }
        return {};
    }
    return QStringLiteral("拒绝删除目录");
}

} // namespace

QString rootRefusal(const QString &path, QString *usablePath)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
        return QStringLiteral("请填写要扫描的绝对路径目录");
    if (!QDir::isAbsolutePath(trimmed))
        return QStringLiteral("目录必须是绝对路径");

    const QString cleaned = QDir::cleanPath(trimmed);
    if (cleaned == QLatin1String("/"))
        return QStringLiteral("不能选择文件系统根目录");

    const QFileInfo info(cleaned);
    if (!info.exists())
        return QStringLiteral("目录不存在");
    if (!info.isDir())
        return QStringLiteral("不是目录");

    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty())
        return QStringLiteral("无法解析目录");
    if (canonical == QLatin1String("/"))
        return QStringLiteral("不能选择文件系统根目录");

    if (usablePath)
        *usablePath = cleaned;
    return {};
}

TreeWalk walkTree(const QString &rootPath, const QVector<Term> &terms,
                  const std::atomic<bool> &cancel, int paceMs,
                  const std::function<void(int files, int issues)> &progress)
{
    TreeWalk report;
    QString usable;
    report.error = rootRefusal(rootPath, &usable);
    if (!report.error.isEmpty())
        return report;
    report.rootPath = usable;

    if (paused(cancel, paceMs < 0 ? 0 : paceMs)) {
        report.cancelled = true;
        return report;
    }

    const int opened = ::open(QFile::encodeName(usable).constData(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (opened < 0) {
        report.issues.append(
            TreeIssue{usable, QStringLiteral("无法读取目录：%1").arg(errorText(errno))});
        if (progress)
            progress(0, report.issues.size());
        return report;
    }

    struct stat rootStat;
    if (::fstat(opened, &rootStat) != 0) {
        const int err = errno;
        ::close(opened);
        report.issues.append(
            TreeIssue{usable, QStringLiteral("无法读取目录：%1").arg(errorText(err))});
        if (progress)
            progress(0, report.issues.size());
        return report;
    }

    OwnedFd climb(::dup(opened));
    if (climb.get() < 0) {
        const int err = errno;
        ::close(opened);
        report.issues.append(
            TreeIssue{usable, QStringLiteral("无法读取目录：%1").arg(errorText(err))});
        return report;
    }

    std::vector<Frame> pending;
    pending.push_back(Frame(OwnedFd(opened), QString(), std::vector<NodeId>{NodeId{rootStat.st_dev, rootStat.st_ino}}));

    WalkContext ctx;
    ctx.terms = &terms;
    ctx.cancel = &cancel;
    ctx.progress = progress;
    ctx.rootPath = usable;
    ctx.rootFd = climb.get();
    ctx.report = &report;
    ctx.pending = &pending;

    while (!pending.empty()) {
        if (stopRequested(cancel)) {
            report.cancelled = true;
            break;
        }
        Frame frame = std::move(pending.back());
        pending.pop_back();
        readDirectory(ctx, frame);
        if (report.cancelled)
            break;
    }
    return report;
}

RemovalBatch removeListed(const QString &rootPath, const QStringList &absolutePaths,
                          const std::atomic<bool> &cancel, int paceMs,
                          const std::function<void(int done, int total)> &progress)
{
    RemovalBatch batch;
    const QString refusal = rootRefusal(rootPath, nullptr);
    if (!refusal.isEmpty()) {
        batch.error = refusal;
        return batch;
    }

    const int delay = paceMs < 0 ? 0 : paceMs;
    if (paused(cancel, delay)) {
        batch.cancelled = true;
        return batch;
    }

    const int total = absolutePaths.size();
    for (int index = 0; index < absolutePaths.size(); ++index) {
        if (index > 0 && paused(cancel, delay)) {
            batch.cancelled = true;
            break;
        }
        if (stopRequested(cancel)) {
            batch.cancelled = true;
            break;
        }

        RemovalNote note;
        note.absolutePath = absolutePaths.at(index);
        note.error = unlinkInside(rootPath, note.absolutePath);
        note.removed = note.error.isEmpty();
        batch.items.append(note);
        if (progress)
            progress(index + 1, total);
    }
    return batch;
}

} // namespace tw
