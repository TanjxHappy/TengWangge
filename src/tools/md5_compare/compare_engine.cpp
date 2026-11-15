#include "tools/md5_compare/compare_engine.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QStringList>

#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>

namespace tw {
namespace {

constexpr int kReadChunkBytes = 256 * 1024;

struct InodeId {
    quint64 dev = 0;
    quint64 ino = 0;

    bool operator==(const InodeId &other) const { return dev == other.dev && ino == other.ino; }
};

QString errnoText()
{
    return QString::fromLocal8Bit(std::strerror(errno));
}

QByteArray encoded(const QString &path)
{
    return QFile::encodeName(path);
}

bool readLinkStat(const QString &path, struct stat *out, bool follow, QString *error)
{
    const QByteArray name = encoded(path);
    const int rc = follow ? ::stat(name.constData(), out) : ::lstat(name.constData(), out);
    if (rc != 0) {
        const int saved = errno;
        if (error)
            *error = QString::fromLocal8Bit(std::strerror(saved));
        errno = saved;
        return false;
    }
    return true;
}

bool inodeOf(const QString &path, InodeId *id, QString *error)
{
    struct stat st;
    if (!readLinkStat(path, &st, true, error))
        return false;
    id->dev = static_cast<quint64>(st.st_dev);
    id->ino = static_cast<quint64>(st.st_ino);
    return true;
}

bool stackContains(const QVector<InodeId> &stack, const InodeId &id)
{
    for (const InodeId &item : stack) {
        if (item == id)
            return true;
    }
    return false;
}

QString canonicalPath(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
}

bool isInsideOrEqual(const QString &rootCanonical, const QString &candidateCanonical)
{
    if (rootCanonical.isEmpty() || candidateCanonical.isEmpty())
        return false;
    if (candidateCanonical == rootCanonical)
        return true;
    const QString prefix = rootCanonical.endsWith(QLatin1Char('/'))
                               ? rootCanonical
                               : rootCanonical + QLatin1Char('/');
    return candidateCanonical.startsWith(prefix);
}

QString joinRel(const QString &prefix, const QString &name)
{
    if (prefix.isEmpty())
        return name;
    return prefix + QLatin1Char('/') + name;
}

QString sideLabel(const QString &side)
{
    if (side == QLatin1String("A"))
        return QStringLiteral("目录 A");
    if (side == QLatin1String("B"))
        return QStringLiteral("目录 B");
    return side;
}

struct ListedWalk {
    QMap<QString, QString> files;
    QString rootCanonical;
    QVector<CompareProblem> problems;
    bool cancelled = false;
    bool truncated = false;
};

struct Frame {
    QString path;
    QString relPrefix;
    QVector<InodeId> stack;
};

struct DirCloser {
    DIR *dir = nullptr;
    ~DirCloser()
    {
        if (dir)
            ::closedir(dir);
    }
    DirCloser(const DirCloser &) = delete;
    DirCloser &operator=(const DirCloser &) = delete;
    explicit DirCloser(DIR *handle)
        : dir(handle)
    {
    }
};

void tally(CompareResult *result)
{
    result->sameCount = 0;
    result->differentCount = 0;
    result->onlyInACount = 0;
    result->onlyInBCount = 0;
    for (const CompareEntry &entry : result->entries) {
        switch (entry.status) {
        case CompareStatus::Same:
            ++result->sameCount;
            break;
        case CompareStatus::Different:
            ++result->differentCount;
            break;
        case CompareStatus::OnlyInA:
            ++result->onlyInACount;
            break;
        case CompareStatus::OnlyInB:
            ++result->onlyInBCount;
            break;
        case CompareStatus::Problem:
            break;
        }
    }
    result->problemCount = result->problems.size();
}

bool cancelledNow(const CompareCancelCallback &shouldCancel)
{
    return static_cast<bool>(shouldCancel) && shouldCancel();
}

void reportProgress(const CompareProgressCallback &progress, bool enumerating, int completed, int total,
                    const QString &path)
{
    if (!progress)
        return;
    CompareProgress update;
    update.enumerating = enumerating;
    update.completed = completed;
    update.total = total;
    update.currentPath = path;
    progress(update);
}

ListedWalk walkDirectory(const QString &side, const QString &rootAbs, const QString &rootCanonical,
                         const CompareProgressCallback &progress, const CompareCancelCallback &shouldCancel,
                         const CompareLimits &limits)
{
    ListedWalk walk;
    walk.rootCanonical = rootCanonical;

    InodeId rootId;
    QString inodeError;
    if (!inodeOf(rootCanonical, &rootId, &inodeError)) {
        walk.problems.append(
            {side, QString(), QStringLiteral("无法读取目录：%1").arg(inodeError)});
        walk.truncated = true;
        return walk;
    }

    bool stop = false;
    auto noteLimit = [&](const QString &message) {
        if (walk.truncated)
            return;
        walk.truncated = true;
        walk.problems.append({side, QString(), message});
        stop = true;
    };

    QVector<Frame> pending;
    pending.append(Frame{rootAbs, QString(), QVector<InodeId>{rootId}});

    while (!pending.isEmpty() && !stop) {
        if (cancelledNow(shouldCancel)) {
            walk.cancelled = true;
            return walk;
        }
        const Frame frame = pending.takeLast();
        reportProgress(progress, true, walk.files.size(), 0,
                       frame.relPrefix.isEmpty() ? rootAbs : frame.relPrefix);
        if (cancelledNow(shouldCancel)) {
            walk.cancelled = true;
            return walk;
        }

        DirCloser handle(::opendir(encoded(frame.path).constData()));
        if (!handle.dir) {
            walk.problems.append(
                {side, frame.relPrefix, QStringLiteral("无法读取目录：%1").arg(errnoText())});
            if (walk.files.size() + walk.problems.size() >= limits.maxEntries)
                noteLimit(QStringLiteral("已达到扫描上限，其余项目未列入比较"));
            continue;
        }

        QStringList names;
        while (dirent *entry = ::readdir(handle.dir)) {
            const QString name = QFile::decodeName(entry->d_name);
            if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
                continue;
            names.append(name);
        }
        names.sort(Qt::CaseSensitive);

        for (const QString &name : names) {
            if (cancelledNow(shouldCancel)) {
                walk.cancelled = true;
                return walk;
            }
            if (walk.files.size() + walk.problems.size() >= limits.maxEntries) {
                noteLimit(QStringLiteral("已达到扫描上限，其余项目未列入比较"));
                break;
            }

            const QString childAbs = QDir(frame.path).filePath(name);
            const QString childRel = joinRel(frame.relPrefix, name);
            struct stat st;
            QString statError;
            if (!readLinkStat(childAbs, &st, false, &statError)) {
                walk.problems.append(
                    {side, childRel, QStringLiteral("无法读取：%1").arg(statError)});
                continue;
            }

            if (S_ISLNK(st.st_mode)) {
                const QString target = canonicalPath(childAbs);
                if (target.isEmpty()) {
                    walk.problems.append(
                        {side, childRel,
                         QStringLiteral("符号链接无法解析（断开、循环或无法访问）")});
                    continue;
                }
                if (!isInsideOrEqual(rootCanonical, target)) {
                    walk.problems.append(
                        {side, childRel, QStringLiteral("符号链接指向起始目录之外，已跳过")});
                    continue;
                }
                struct stat targetStat;
                QString targetError;
                if (!readLinkStat(target, &targetStat, true, &targetError)) {
                    const QString message = errno == ELOOP
                                                ? QStringLiteral("符号链接会形成循环，已跳过")
                                                : QStringLiteral("无法读取：%1").arg(targetError);
                    walk.problems.append({side, childRel, message});
                    continue;
                }
                if (S_ISDIR(targetStat.st_mode)) {
                    const InodeId id{static_cast<quint64>(targetStat.st_dev),
                                     static_cast<quint64>(targetStat.st_ino)};
                    if (stackContains(frame.stack, id)) {
                        walk.problems.append(
                            {side, childRel, QStringLiteral("符号链接会形成循环，已跳过")});
                        continue;
                    }
                    if (frame.stack.size() >= limits.maxDepth) {
                        walk.problems.append(
                            {side, childRel, QStringLiteral("目录层级过深，已跳过")});
                        continue;
                    }
                    Frame next;
                    next.path = target;
                    next.relPrefix = childRel;
                    next.stack = frame.stack;
                    next.stack.append(id);
                    pending.append(next);
                    continue;
                }
                if (S_ISREG(targetStat.st_mode)) {
                    if (walk.files.size() >= limits.maxFiles) {
                        noteLimit(QStringLiteral("已达到扫描上限，其余项目未列入比较"));
                        break;
                    }
                    walk.files.insert(childRel, childAbs);
                    continue;
                }
                walk.problems.append(
                    {side, childRel, QStringLiteral("符号链接指向特殊文件，已跳过")});
                continue;
            }

            if (S_ISDIR(st.st_mode)) {
                const QString target = canonicalPath(childAbs);
                if (target.isEmpty() || !isInsideOrEqual(rootCanonical, target)) {
                    walk.problems.append(
                        {side, childRel, QStringLiteral("目录位于起始目录之外，已跳过")});
                    continue;
                }
                const InodeId id{static_cast<quint64>(st.st_dev), static_cast<quint64>(st.st_ino)};
                if (stackContains(frame.stack, id)) {
                    walk.problems.append(
                        {side, childRel, QStringLiteral("符号链接会形成循环，已跳过")});
                    continue;
                }
                if (frame.stack.size() >= limits.maxDepth) {
                    walk.problems.append({side, childRel, QStringLiteral("目录层级过深，已跳过")});
                    continue;
                }
                Frame next;
                next.path = childAbs;
                next.relPrefix = childRel;
                next.stack = frame.stack;
                next.stack.append(id);
                pending.append(next);
                continue;
            }

            if (S_ISREG(st.st_mode)) {
                if (walk.files.size() >= limits.maxFiles) {
                    noteLimit(QStringLiteral("已达到扫描上限，其余项目未列入比较"));
                    break;
                }
                walk.files.insert(childRel, childAbs);
                continue;
            }

            walk.problems.append({side, childRel, QStringLiteral("不是普通文件，已跳过")});
        }
    }
    return walk;
}

QString csvQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString existingCanonicalAncestor(const QString &path)
{
    QString current = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    while (!current.isEmpty()) {
        const QFileInfo info(current);
        if (info.exists() || info.isSymLink()) {
            const QString canon = info.canonicalFilePath();
            return canon;
        }
        if (current == QLatin1String("/"))
            break;
        const QString parent = QFileInfo(current).absolutePath();
        if (parent == current)
            break;
        current = parent;
    }
    return QStringLiteral("/");
}

} // namespace

QString compareStatusLabel(CompareStatus status)
{
    switch (status) {
    case CompareStatus::Same:
        return QStringLiteral("相同");
    case CompareStatus::Different:
        return QStringLiteral("内容不同");
    case CompareStatus::OnlyInA:
        return QStringLiteral("仅在目录 A");
    case CompareStatus::OnlyInB:
        return QStringLiteral("仅在目录 B");
    case CompareStatus::Problem:
        return QStringLiteral("问题");
    }
    return QStringLiteral("问题");
}

bool compareEntryMatches(const CompareEntry &entry, int statusFilter, const QString &query)
{
    if (statusFilter >= 0 && static_cast<int>(entry.status) != statusFilter)
        return false;
    const QString needle = query.trimmed();
    if (needle.isEmpty())
        return true;
    return entry.relativePath.contains(needle, Qt::CaseInsensitive);
}

FileHashStatus hashFileMd5(const QString &absolutePath, const QString &rootCanonical, QString *hex,
                           QString *error, const CompareCancelCallback &shouldCancel)
{
    if (hex)
        hex->clear();
    if (error)
        error->clear();
    auto fail = [&](const QString &message) {
        if (hex)
            hex->clear();
        if (error)
            *error = message;
        return FileHashStatus::Failed;
    };
    auto cancel = [&]() {
        if (hex)
            hex->clear();
        return FileHashStatus::Cancelled;
    };
    if (cancelledNow(shouldCancel))
        return cancel();

    struct stat st;
    QString statError;
    if (!readLinkStat(absolutePath, &st, false, &statError))
        return fail(QStringLiteral("无法读取：%1").arg(statError));

    QString pathToRead = absolutePath;
    if (S_ISLNK(st.st_mode)) {
        const QString target = canonicalPath(absolutePath);
        if (target.isEmpty() || !isInsideOrEqual(rootCanonical, target))
            return fail(QStringLiteral("符号链接无效或指向起始目录之外"));
        struct stat targetStat;
        QString targetError;
        if (!readLinkStat(target, &targetStat, true, &targetError) || !S_ISREG(targetStat.st_mode))
            return fail(QStringLiteral("符号链接目标不是普通文件"));
        pathToRead = target;
    } else if (!S_ISREG(st.st_mode)) {
        return fail(QStringLiteral("不是普通文件"));
    } else {
        const QString target = canonicalPath(absolutePath);
        if (target.isEmpty() || !isInsideOrEqual(rootCanonical, target))
            return fail(QStringLiteral("文件位于起始目录之外"));
        pathToRead = target;
    }

    QFile file(pathToRead);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法读取：%1").arg(file.errorString()));

    QCryptographicHash hash(QCryptographicHash::Md5);
    QByteArray buffer(kReadChunkBytes, Qt::Uninitialized);
    while (true) {
        if (cancelledNow(shouldCancel))
            return cancel();
        const qint64 n = file.read(buffer.data(), buffer.size());
        if (n < 0)
            return fail(QStringLiteral("无法读取：%1").arg(file.errorString()));
        if (n == 0)
            break;
        hash.addData(buffer.constData(), static_cast<int>(n));
    }
    if (hex)
        *hex = QString::fromLatin1(hash.result().toHex());
    return FileHashStatus::Ok;
}

bool compareDirectories(const QString &dirA, const QString &dirB, CompareResult *result, QString *error,
                        const CompareProgressCallback &progress, const CompareCancelCallback &shouldCancel,
                        CompareLimits limits)
{
    if (error)
        error->clear();
    if (!result) {
        if (error)
            *error = QStringLiteral("内部错误：结果对象为空");
        return false;
    }
    *result = {};

    if (limits.maxDepth < 1)
        limits.maxDepth = 1;
    if (limits.maxFiles < 1)
        limits.maxFiles = 1;
    if (limits.maxEntries < 1)
        limits.maxEntries = 1;

    auto reject = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto markCancelled = [&]() {
        *result = {};
        result->cancelled = true;
        return true;
    };
    if (cancelledNow(shouldCancel))
        return markCancelled();

    const auto describeRoot = [&](const QString &label, const QString &dir, QString *abs,
                                  QString *canon) {
        const QString trimmed = dir.trimmed();
        if (trimmed.isEmpty())
            return reject(QStringLiteral("请选择%1").arg(label));
        const QFileInfo info(trimmed);
        if (!info.exists())
            return reject(QStringLiteral("%1不存在：%2").arg(label, trimmed));
        if (!info.isDir())
            return reject(QStringLiteral("%1不是目录：%2").arg(label, trimmed));
        *abs = info.absoluteFilePath();
        *canon = info.canonicalFilePath();
        if (canon->isEmpty())
            return reject(QStringLiteral("无法解析%1：%2").arg(label, trimmed));
        return true;
    };

    QString absA;
    QString canonA;
    QString absB;
    QString canonB;
    if (!describeRoot(QStringLiteral("目录 A"), dirA, &absA, &canonA))
        return false;
    if (cancelledNow(shouldCancel))
        return markCancelled();
    if (!describeRoot(QStringLiteral("目录 B"), dirB, &absB, &canonB))
        return false;

    const ListedWalk walkA = walkDirectory(QStringLiteral("A"), absA, canonA, progress, shouldCancel, limits);
    if (walkA.cancelled)
        return markCancelled();
    const ListedWalk walkB = walkDirectory(QStringLiteral("B"), absB, canonB, progress, shouldCancel, limits);
    if (walkB.cancelled)
        return markCancelled();

    const auto rootFailure = [](const ListedWalk &walk) -> QString {
        for (const CompareProblem &problem : walk.problems) {
            if (problem.relativePath.isEmpty()
                && problem.message.startsWith(QStringLiteral("无法读取目录")))
                return problem.message;
        }
        return {};
    };
    const QString rootErrorA = rootFailure(walkA);
    if (!rootErrorA.isEmpty())
        return reject(QStringLiteral("目录 A：%1").arg(rootErrorA));
    const QString rootErrorB = rootFailure(walkB);
    if (!rootErrorB.isEmpty())
        return reject(QStringLiteral("目录 B：%1").arg(rootErrorB));

    QMap<QString, QStringList> details;
    QSet<QString> paths;
    auto takeProblems = [&](const ListedWalk &walk) {
        for (const CompareProblem &problem : walk.problems) {
            result->problems.append(problem);
            details[problem.relativePath].append(
                QStringLiteral("%1：%2").arg(sideLabel(problem.side), problem.message));
            paths.insert(problem.relativePath);
        }
    };
    takeProblems(walkA);
    takeProblems(walkB);
    for (auto it = walkA.files.cbegin(); it != walkA.files.cend(); ++it)
        paths.insert(it.key());
    for (auto it = walkB.files.cbegin(); it != walkB.files.cend(); ++it)
        paths.insert(it.key());

    QStringList ordered = paths.values();
    ordered.sort(Qt::CaseSensitive);

    int totalReads = 0;
    for (const QString &path : ordered) {
        if (walkA.files.contains(path))
            ++totalReads;
        if (walkB.files.contains(path))
            ++totalReads;
    }

    int done = 0;
    for (const QString &path : ordered) {
        if (cancelledNow(shouldCancel))
            return markCancelled();

        CompareEntry entry;
        entry.relativePath = path;
        const bool inA = walkA.files.contains(path);
        const bool inB = walkB.files.contains(path);
        const bool hadProblem = details.contains(path);
        bool failedA = false;
        bool failedB = false;

        if (inA) {
            QString hashError;
            const FileHashStatus status = hashFileMd5(walkA.files.value(path), walkA.rootCanonical,
                                                      &entry.md5A, &hashError, shouldCancel);
            if (status == FileHashStatus::Cancelled)
                return markCancelled();
            ++done;
            reportProgress(progress, false, done, totalReads, path);
            if (status != FileHashStatus::Ok) {
                failedA = true;
                entry.md5A.clear();
                result->problems.append({QStringLiteral("A"), path, hashError});
                details[path].append(QStringLiteral("目录 A：%1").arg(hashError));
            }
        }
        if (cancelledNow(shouldCancel))
            return markCancelled();
        if (inB) {
            QString hashError;
            const FileHashStatus status = hashFileMd5(walkB.files.value(path), walkB.rootCanonical,
                                                      &entry.md5B, &hashError, shouldCancel);
            if (status == FileHashStatus::Cancelled)
                return markCancelled();
            ++done;
            reportProgress(progress, false, done, totalReads, path);
            if (status != FileHashStatus::Ok) {
                failedB = true;
                entry.md5B.clear();
                result->problems.append({QStringLiteral("B"), path, hashError});
                details[path].append(QStringLiteral("目录 B：%1").arg(hashError));
            }
        }

        if (hadProblem || failedA || failedB) {
            entry.status = CompareStatus::Problem;
            entry.detail = details.value(path).join(QStringLiteral("；"));
        } else if (inA && inB) {
            entry.status = entry.md5A == entry.md5B ? CompareStatus::Same : CompareStatus::Different;
        } else if (inA) {
            entry.status = CompareStatus::OnlyInA;
        } else if (inB) {
            entry.status = CompareStatus::OnlyInB;
        } else {
            entry.status = CompareStatus::Problem;
            entry.detail = details.value(path).join(QStringLiteral("；"));
        }
        result->entries.append(entry);
    }

    result->truncated = walkA.truncated || walkB.truncated;
    tally(result);
    return true;
}

bool writeCompareReport(const QString &filePath, const CompareResult &result, QString *error)
{
    if (error)
        error->clear();
    const QString path = filePath.trimmed();
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("请选择要导出的文件");
        return false;
    }
    const QFileInfo info(path);
    if (info.exists() && info.isDir()) {
        if (error)
            *error = QStringLiteral("导出路径是一个目录");
        return false;
    }
    const QString parent = QFileInfo(path).absolutePath();
    if (!QFileInfo(parent).isDir()) {
        if (error)
            *error = QStringLiteral("导出目录不存在：%1").arg(parent);
        return false;
    }

    QStringList lines;
    lines.append(QStringList{csvQuote(QStringLiteral("相对路径")), csvQuote(QStringLiteral("状态")),
                             csvQuote(QStringLiteral("目录A的MD5")), csvQuote(QStringLiteral("目录B的MD5")),
                             csvQuote(QStringLiteral("说明"))}
                     .join(QLatin1Char(',')));
    for (const CompareEntry &entry : result.entries) {
        lines.append(QStringList{csvQuote(entry.relativePath), csvQuote(compareStatusLabel(entry.status)),
                                 csvQuote(entry.md5A), csvQuote(entry.md5B), csvQuote(entry.detail)}
                         .join(QLatin1Char(',')));
    }
    const QByteArray payload = QByteArray("\xEF\xBB\xBF") + lines.join(QLatin1Char('\n')).toUtf8()
                               + QByteArray("\n");

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入报告：%1").arg(file.errorString());
        return false;
    }
    if (file.write(payload) != payload.size()) {
        if (error)
            *error = QStringLiteral("无法写入报告：%1").arg(file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("无法写入报告：%1").arg(file.errorString());
        return false;
    }
    return true;
}

QString compareReportBlockReason(const QString &dirA, const QString &dirB, const QString &reportPath)
{
    const QString trimmed = reportPath.trimmed();
    if (trimmed.isEmpty())
        return QStringLiteral("请选择要导出的文件");
    const QFileInfo info(trimmed);
    if (info.exists() && info.isDir())
        return QStringLiteral("导出路径是一个目录");

    const QString anchor = existingCanonicalAncestor(trimmed);
    if (anchor.isEmpty())
        return QStringLiteral("无法解析导出路径");

    const auto blocked = [&](const QString &dir) {
        const QString root = QFileInfo(dir.trimmed()).canonicalFilePath();
        return !root.isEmpty() && isInsideOrEqual(root, anchor);
    };
    if (blocked(dirA) || blocked(dirB))
        return QStringLiteral("不能把报告写进正在比较的目录");
    return {};
}

} // namespace tw
