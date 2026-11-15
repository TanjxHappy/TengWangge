#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "tools/md5_compare_high/high_digest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSaveFile>
#include <QStringList>

#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <utility>

namespace tw {
namespace {

constexpr int kReadChunkBytes = 64 * 1024;

struct NodeId {
    quint64 device = 0;
    quint64 inode = 0;

    bool operator==(const NodeId &other) const
    {
        return device == other.device && inode == other.inode;
    }
};

struct DirCloser {
    DIR *dir = nullptr;
    explicit DirCloser(DIR *handle)
        : dir(handle)
    {
    }
    ~DirCloser()
    {
        if (dir)
            ::closedir(dir);
    }
    DirCloser(const DirCloser &) = delete;
    DirCloser &operator=(const DirCloser &) = delete;
};

struct FdCloser {
    int fd = -1;
    explicit FdCloser(int handle)
        : fd(handle)
    {
    }
    ~FdCloser()
    {
        if (fd >= 0)
            ::close(fd);
    }
    FdCloser(const FdCloser &) = delete;
    FdCloser &operator=(const FdCloser &) = delete;
};

QString errnoText(int err)
{
    return QString::fromStdString(std::generic_category().message(err));
}

QByteArray localBytes(const QString &path)
{
    return QFile::encodeName(path);
}

QString joinPath(const QString &prefix, const QString &name)
{
    if (prefix.isEmpty())
        return name;
    if (prefix.endsWith(QLatin1Char('/')))
        return prefix + name;
    return prefix + QLatin1Char('/') + name;
}

bool withinRoot(const QString &rootCanonical, const QString &candidateCanonical)
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

NodeId nodeOf(const struct stat &status)
{
    NodeId id;
    id.device = static_cast<quint64>(status.st_dev);
    id.inode = static_cast<quint64>(status.st_ino);
    return id;
}

bool containsNode(const QVector<NodeId> &nodes, const NodeId &id)
{
    for (const NodeId &node : nodes) {
        if (node == id)
            return true;
    }
    return false;
}

bool readStatus(const QString &path, struct stat *out, bool follow, QString *error)
{
    const QByteArray encoded = localBytes(path);
    const int rc = follow ? ::stat(encoded.constData(), out) : ::lstat(encoded.constData(), out);
    if (rc == 0)
        return true;
    if (error) {
        const int err = errno;
        *error = errnoText(err);
    }
    return false;
}

HighIssue makeIssue(const QString &side, const QString &path, const QString &message)
{
    HighIssue issue;
    issue.side = side;
    issue.relativePath = path;
    issue.message = message;
    return issue;
}

QString sidePrefix(const QString &side)
{
    if (side == QLatin1String("B"))
        return QStringLiteral("目录 B：");
    return QStringLiteral("目录 A：");
}

bool stopNow(const HighStopFn &stopRequested)
{
    return static_cast<bool>(stopRequested) && stopRequested();
}

struct Inventory {
    QMap<QString, QString> files;
    QVector<HighIssue> issues;
    bool cancelled = false;
    bool reachedCap = false;
    QString fatalError;
};

struct WalkFrame {
    QString absolute;
    QString relative;
    QVector<NodeId> ancestors;
};

class TreeCollector {
public:
    TreeCollector(const QString &side, const QString &label, const QString &rootCanonical, HighCaps caps,
                  HighProgressFn progress, HighStopFn stopRequested)
        : m_side(side)
        , m_label(label)
        , m_root(rootCanonical)
        , m_caps(std::move(caps))
        , m_progress(std::move(progress))
        , m_stop(std::move(stopRequested))
    {
    }

    Inventory collect()
    {
        Inventory inventory;
        if (stopNow(m_stop)) {
            inventory.cancelled = true;
            return inventory;
        }

        struct stat rootStatus;
        QString statError;
        if (!readStatus(m_root, &rootStatus, false, &statError) || !S_ISDIR(rootStatus.st_mode)) {
            inventory.fatalError = QStringLiteral("无法读取%1：%2")
                                       .arg(m_label, statError.isEmpty() ? QStringLiteral("不是目录") : statError);
            return inventory;
        }

        QVector<WalkFrame> pending;
        WalkFrame rootFrame;
        rootFrame.absolute = m_root;
        rootFrame.ancestors.append(nodeOf(rootStatus));
        pending.append(rootFrame);

        while (!pending.isEmpty() && !inventory.cancelled && !inventory.reachedCap) {
            if (stopNow(m_stop)) {
                inventory.cancelled = true;
                break;
            }
            const WalkFrame frame = pending.takeLast();
            notify(frame, inventory.files.size());
            if (stopNow(m_stop)) {
                inventory.cancelled = true;
                break;
            }

            QVector<WalkFrame> children;
            if (!consume(frame, &inventory, &children))
                break;
            if (inventory.cancelled || inventory.reachedCap)
                break;
            for (int index = children.size() - 1; index >= 0; --index)
                pending.append(children.at(index));
        }
        return inventory;
    }

private:
    void notify(const WalkFrame &frame, int done) const
    {
        if (!m_progress)
            return;
        HighProgress progress;
        progress.scanning = true;
        progress.done = done;
        progress.total = 0;
        progress.path = frame.relative.isEmpty() ? m_root : frame.relative;
        m_progress(progress);
    }

    void noteCap(Inventory *inventory) const
    {
        if (inventory->reachedCap)
            return;
        inventory->reachedCap = true;
        inventory->issues.append(
            makeIssue(m_side, QString(), QStringLiteral("已达到扫描上限，其余内容未继续比较")));
    }

    bool overItemBudget(const Inventory &inventory) const
    {
        return inventory.files.size() + inventory.issues.size() >= m_caps.items;
    }

    bool consume(const WalkFrame &frame, Inventory *inventory, QVector<WalkFrame> *children)
    {
        const QByteArray encoded = localBytes(frame.absolute);
        DirCloser handle(::opendir(encoded.constData()));
        if (!handle.dir) {
            const int err = errno;
            if (frame.relative.isEmpty()) {
                inventory->fatalError = QStringLiteral("无法读取%1：%2").arg(m_label, errnoText(err));
                return false;
            }
            inventory->issues.append(
                makeIssue(m_side, frame.relative, QStringLiteral("无法读取：%1").arg(errnoText(err))));
            return true;
        }

        QStringList names;
        errno = 0;
        while (dirent *entry = ::readdir(handle.dir)) {
            const QString name = QFile::decodeName(entry->d_name);
            if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
                continue;
            names.append(name);
        }
        const int readError = errno;
        ::closedir(handle.dir);
        handle.dir = nullptr;
        if (readError != 0) {
            const QString message = QStringLiteral("无法读取：%1").arg(errnoText(readError));
            inventory->issues.append(makeIssue(m_side, frame.relative, message));
        }
        names.sort(Qt::CaseSensitive);

        for (const QString &name : names) {
            if (stopNow(m_stop)) {
                inventory->cancelled = true;
                children->clear();
                return true;
            }
            if (overItemBudget(*inventory)) {
                noteCap(inventory);
                children->clear();
                return true;
            }
            const QString childAbs = joinPath(frame.absolute, name);
            const QString childRel = joinPath(frame.relative, name);
            considerChild(frame, childAbs, childRel, inventory, children);
            if (inventory->cancelled || inventory->reachedCap) {
                children->clear();
                return true;
            }
        }
        return true;
    }

    void considerChild(const WalkFrame &frame, const QString &childAbs, const QString &childRel,
                       Inventory *inventory, QVector<WalkFrame> *children) const
    {
        struct stat status;
        QString statError;
        if (!readStatus(childAbs, &status, false, &statError)) {
            inventory->issues.append(
                makeIssue(m_side, childRel, QStringLiteral("无法读取：%1").arg(statError)));
            return;
        }

        if (S_ISLNK(status.st_mode)) {
            considerLink(frame, childAbs, childRel, inventory, children);
            return;
        }
        if (S_ISDIR(status.st_mode)) {
            considerDirectory(frame, childAbs, childRel, nodeOf(status), inventory, children,
                              QStringLiteral("目录位于起始目录之外，未进入"));
            return;
        }
        if (S_ISREG(status.st_mode)) {
            considerFile(childAbs, childRel, inventory);
            return;
        }
        inventory->issues.append(makeIssue(m_side, childRel, QStringLiteral("不是普通文件，已跳过")));
    }

    void considerLink(const WalkFrame &frame, const QString &childAbs, const QString &childRel,
                      Inventory *inventory, QVector<WalkFrame> *children) const
    {
        const QString target = QFileInfo(childAbs).canonicalFilePath();
        if (target.isEmpty()) {
            struct stat ignored;
            const QByteArray encoded = localBytes(childAbs);
            const bool loop = ::stat(encoded.constData(), &ignored) != 0 && errno == ELOOP;
            inventory->issues.append(makeIssue(
                m_side, childRel,
                loop ? QStringLiteral("符号链接会形成循环，未跟随")
                     : QStringLiteral("符号链接无法解析，未跟随")));
            return;
        }
        if (!withinRoot(m_root, target)) {
            inventory->issues.append(
                makeIssue(m_side, childRel, QStringLiteral("符号链接指向起始目录之外，未跟随")));
            return;
        }

        struct stat targetStatus;
        QString targetError;
        if (!readStatus(target, &targetStatus, true, &targetError)) {
            struct stat ignored;
            const bool loop = ::stat(localBytes(target).constData(), &ignored) != 0 && errno == ELOOP;
            inventory->issues.append(makeIssue(
                m_side, childRel,
                loop ? QStringLiteral("符号链接会形成循环，未跟随")
                     : QStringLiteral("无法读取：%1").arg(targetError)));
            return;
        }
        if (S_ISDIR(targetStatus.st_mode)) {
            considerDirectory(frame, childAbs, childRel, nodeOf(targetStatus), inventory, children,
                              QStringLiteral("符号链接指向起始目录之外，未跟随"));
            return;
        }
        if (S_ISREG(targetStatus.st_mode)) {
            considerFile(childAbs, childRel, inventory);
            return;
        }
        inventory->issues.append(makeIssue(m_side, childRel, QStringLiteral("不是普通文件，已跳过")));
    }

    void considerDirectory(const WalkFrame &frame, const QString &childAbs, const QString &childRel,
                           const NodeId &id, Inventory *inventory, QVector<WalkFrame> *children,
                           const QString &outsideMessage) const
    {
        const QString canonical = QFileInfo(childAbs).canonicalFilePath();
        if (canonical.isEmpty() || !withinRoot(m_root, canonical)) {
            inventory->issues.append(makeIssue(m_side, childRel, outsideMessage));
            return;
        }
        if (containsNode(frame.ancestors, id)) {
            inventory->issues.append(makeIssue(m_side, childRel, QStringLiteral("符号链接会形成循环，未跟随")));
            return;
        }
        if (frame.ancestors.size() >= m_caps.depth) {
            inventory->issues.append(makeIssue(m_side, childRel, QStringLiteral("目录层级已达上限，未继续进入")));
            return;
        }
        WalkFrame next;
        next.absolute = childAbs;
        next.relative = childRel;
        next.ancestors = frame.ancestors;
        next.ancestors.append(id);
        children->append(next);
    }

    void considerFile(const QString &childAbs, const QString &childRel, Inventory *inventory) const
    {
        const QString canonical = QFileInfo(childAbs).canonicalFilePath();
        if (canonical.isEmpty() || !withinRoot(m_root, canonical)) {
            inventory->issues.append(makeIssue(m_side, childRel, QStringLiteral("文件位于起始目录之外，已跳过")));
            return;
        }
        if (inventory->files.size() >= m_caps.files) {
            noteCap(inventory);
            return;
        }
        inventory->files.insert(childRel, childAbs);
    }

    QString m_side;
    QString m_label;
    QString m_root;
    HighCaps m_caps;
    HighProgressFn m_progress;
    HighStopFn m_stop;
};

bool resolveRoot(const QString &label, const QString &input, QString *canonical, QString *error)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) {
        if (error)
            *error = QStringLiteral("请选择%1").arg(label);
        return false;
    }
    const QFileInfo info(trimmed);
    if (!info.exists()) {
        if (error)
            *error = QStringLiteral("%1 不存在：%2").arg(label, trimmed);
        return false;
    }
    if (!info.isDir()) {
        if (error)
            *error = QStringLiteral("%1 不是目录：%2").arg(label, trimmed);
        return false;
    }
    const QString resolved = info.canonicalFilePath();
    if (resolved.isEmpty()) {
        if (error)
            *error = QStringLiteral("无法解析%1：%2").arg(label, trimmed);
        return false;
    }
    *canonical = resolved;
    return true;
}

void tally(HighReport *report)
{
    report->matchCount = 0;
    report->mismatchCount = 0;
    report->onlyACount = 0;
    report->onlyBCount = 0;
    for (const HighRow &row : report->rows) {
        switch (row.kind) {
        case HighRowKind::Match:
            ++report->matchCount;
            break;
        case HighRowKind::Mismatch:
            ++report->mismatchCount;
            break;
        case HighRowKind::OnlyA:
            ++report->onlyACount;
            break;
        case HighRowKind::OnlyB:
            ++report->onlyBCount;
            break;
        case HighRowKind::Issue:
            break;
        }
    }
    report->issueCount = report->issues.size();
}

QString quoteCsv(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString canonicalAnchor(const QString &path)
{
    QFileInfo info(QFileInfo(path).absoluteFilePath());
    for (int hop = 0; hop < 512; ++hop) {
        if (info.exists())
            return info.canonicalFilePath();
        const QString parent = info.absolutePath();
        if (parent.isEmpty() || parent == info.absoluteFilePath())
            return {};
        info.setFile(parent);
    }
    return {};
}

bool hashSide(const QMap<QString, QString> &files, const QString &rootCanonical, const QString &side,
              int *done, int total, QMap<QString, QString> *digests, QVector<HighIssue> *issues,
              const HighProgressFn &progress, const HighStopFn &stopRequested)
{
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (stopNow(stopRequested))
            return false;
        if (progress) {
            HighProgress update;
            update.scanning = false;
            update.done = *done;
            update.total = total;
            update.path = it.key();
            progress(update);
        }
        if (stopNow(stopRequested))
            return false;

        QString hex;
        QString hashError;
        const FileMd5State state =
            hashContainedFileMd5(it.value(), rootCanonical, &hex, &hashError, stopRequested);
        if (state == FileMd5State::Stopped)
            return false;
        if (state != FileMd5State::Ready) {
            issues->append(makeIssue(side, it.key(),
                                    hashError.isEmpty() ? QStringLiteral("无法读取") : hashError));
        } else {
            digests->insert(it.key(), hex);
        }
        ++(*done);
    }
    return true;
}

} // namespace

QString highRowKindText(HighRowKind kind)
{
    switch (kind) {
    case HighRowKind::Match:
        return QStringLiteral("相同");
    case HighRowKind::Mismatch:
        return QStringLiteral("内容不同");
    case HighRowKind::OnlyA:
        return QStringLiteral("仅在目录 A");
    case HighRowKind::OnlyB:
        return QStringLiteral("仅在目录 B");
    case HighRowKind::Issue:
        return QStringLiteral("问题");
    }
    return QStringLiteral("问题");
}

bool highRowVisible(const HighRow &row, int kindFilter, const QString &query)
{
    if (kindFilter >= 0 && static_cast<int>(row.kind) != kindFilter)
        return false;
    const QString needle = query.trimmed();
    if (needle.isEmpty())
        return true;
    return row.relativePath.contains(needle, Qt::CaseInsensitive);
}

FileMd5State hashContainedFileMd5(const QString &absolutePath, const QString &rootCanonical, QString *hex,
                                  QString *error, const HighStopFn &stopRequested)
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
        return FileMd5State::Failed;
    };
    auto stop = [&]() {
        if (hex)
            hex->clear();
        return FileMd5State::Stopped;
    };
    if (stopNow(stopRequested))
        return stop();

    struct stat status;
    QString statError;
    if (!readStatus(absolutePath, &status, false, &statError))
        return fail(QStringLiteral("无法读取：%1").arg(statError));

    QString openPath = absolutePath;
    if (S_ISLNK(status.st_mode)) {
        const QString target = QFileInfo(absolutePath).canonicalFilePath();
        if (target.isEmpty() || !withinRoot(rootCanonical, target))
            return fail(QStringLiteral("符号链接无效或指向起始目录之外"));
        struct stat targetStatus;
        QString targetError;
        if (!readStatus(target, &targetStatus, true, &targetError) || !S_ISREG(targetStatus.st_mode))
            return fail(QStringLiteral("符号链接目标不是普通文件"));
        openPath = target;
    } else if (!S_ISREG(status.st_mode)) {
        return fail(QStringLiteral("不是普通文件"));
    } else {
        const QString target = QFileInfo(absolutePath).canonicalFilePath();
        if (target.isEmpty() || !withinRoot(rootCanonical, target))
            return fail(QStringLiteral("文件位于起始目录之外"));
    }

    if (stopNow(stopRequested))
        return stop();

    FdCloser file(::open(localBytes(openPath).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (file.fd < 0)
        return fail(QStringLiteral("无法读取：%1").arg(errnoText(errno)));

    QCryptographicHash hash(QCryptographicHash::Md5);
    QByteArray buffer(kReadChunkBytes, '\0');
    while (true) {
        if (stopNow(stopRequested))
            return stop();
        const ssize_t count = ::read(file.fd, buffer.data(), static_cast<size_t>(buffer.size()));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return fail(QStringLiteral("无法读取：%1").arg(errnoText(errno)));
        }
        if (count == 0)
            break;
        hash.addData(buffer.constData(), static_cast<int>(count));
    }
    if (hex)
        *hex = QString::fromLatin1(hash.result().toHex());
    return FileMd5State::Ready;
}

bool digestDirectoryPair(const QString &dirA, const QString &dirB, HighReport *report, QString *error,
                         const HighProgressFn &progress, const HighStopFn &stopRequested, HighCaps caps)
{
    if (error)
        error->clear();
    if (!report) {
        if (error)
            *error = QStringLiteral("内部错误：结果对象为空");
        return false;
    }
    *report = HighReport();
    if (caps.depth < 1)
        caps.depth = 1;
    if (caps.files < 1)
        caps.files = 1;
    if (caps.items < 1)
        caps.items = 1;

    auto reject = [&](const QString &message) {
        *report = HighReport();
        if (error)
            *error = message;
        return false;
    };
    auto markCancelled = [&]() {
        *report = HighReport();
        report->cancelled = true;
        if (error)
            error->clear();
        return true;
    };
    if (stopNow(stopRequested))
        return markCancelled();

    QString canonA;
    QString canonB;
    QString rootError;
    if (!resolveRoot(QStringLiteral("目录 A"), dirA, &canonA, &rootError))
        return reject(rootError);
    if (stopNow(stopRequested))
        return markCancelled();
    if (!resolveRoot(QStringLiteral("目录 B"), dirB, &canonB, &rootError))
        return reject(rootError);
    if (stopNow(stopRequested))
        return markCancelled();

    TreeCollector collectorA(QStringLiteral("A"), QStringLiteral("目录 A"), canonA, caps, progress, stopRequested);
    const Inventory inventoryA = collectorA.collect();
    if (!inventoryA.fatalError.isEmpty())
        return reject(inventoryA.fatalError);
    if (inventoryA.cancelled)
        return markCancelled();

    TreeCollector collectorB(QStringLiteral("B"), QStringLiteral("目录 B"), canonB, caps, progress, stopRequested);
    const Inventory inventoryB = collectorB.collect();
    if (!inventoryB.fatalError.isEmpty())
        return reject(inventoryB.fatalError);
    if (inventoryB.cancelled)
        return markCancelled();

    const int total = inventoryA.files.size() + inventoryB.files.size();
    int done = 0;
    QMap<QString, QString> digestA;
    QMap<QString, QString> digestB;
    QVector<HighIssue> issues = inventoryA.issues;
    issues += inventoryB.issues;
    if (!hashSide(inventoryA.files, canonA, QStringLiteral("A"), &done, total, &digestA, &issues, progress,
                  stopRequested))
        return markCancelled();
    if (!hashSide(inventoryB.files, canonB, QStringLiteral("B"), &done, total, &digestB, &issues, progress,
                  stopRequested))
        return markCancelled();

    QMap<QString, QStringList> notes;
    QStringList capNotes;
    for (const HighIssue &issue : issues) {
        const QString text = sidePrefix(issue.side) + issue.message;
        if (issue.relativePath.isEmpty())
            capNotes.append(text);
        else
            notes[issue.relativePath].append(text);
    }

    QMap<QString, int> paths;
    for (auto it = digestA.cbegin(); it != digestA.cend(); ++it)
        paths.insert(it.key(), 0);
    for (auto it = digestB.cbegin(); it != digestB.cend(); ++it)
        paths.insert(it.key(), 0);
    for (auto it = notes.cbegin(); it != notes.cend(); ++it)
        paths.insert(it.key(), 0);

    for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
        const QString path = it.key();
        HighRow row;
        row.relativePath = path;
        const bool hasA = digestA.contains(path);
        const bool hasB = digestB.contains(path);
        if (hasA)
            row.md5A = digestA.value(path);
        if (hasB)
            row.md5B = digestB.value(path);
        if (notes.contains(path)) {
            row.kind = HighRowKind::Issue;
            row.note = notes.value(path).join(QStringLiteral("；"));
        } else if (hasA && hasB) {
            row.kind = row.md5A == row.md5B ? HighRowKind::Match : HighRowKind::Mismatch;
        } else if (hasA) {
            row.kind = HighRowKind::OnlyA;
        } else if (hasB) {
            row.kind = HighRowKind::OnlyB;
        } else {
            row.kind = HighRowKind::Issue;
        }
        report->rows.append(row);
    }
    if (!capNotes.isEmpty()) {
        HighRow capRow;
        capRow.relativePath = QStringLiteral("（扫描上限）");
        capRow.kind = HighRowKind::Issue;
        capRow.note = capNotes.join(QStringLiteral("；"));
        report->rows.append(capRow);
    }
    std::sort(report->rows.begin(), report->rows.end(), [](const HighRow &left, const HighRow &right) {
        return left.relativePath < right.relativePath;
    });

    report->issues = issues;
    report->reachedCap = inventoryA.reachedCap || inventoryB.reachedCap;
    report->cancelled = false;
    tally(report);
    return true;
}

bool writeHighReportCsv(const QString &filePath, const HighReport &report, QString *error)
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
    const QString parent = info.absolutePath();
    if (!QFileInfo(parent).isDir()) {
        if (error)
            *error = QStringLiteral("导出目录不存在：%1").arg(parent);
        return false;
    }

    QStringList lines;
    lines.append(QStringList{quoteCsv(QStringLiteral("相对路径")), quoteCsv(QStringLiteral("状态")),
                             quoteCsv(QStringLiteral("目录A的MD5")), quoteCsv(QStringLiteral("目录B的MD5")),
                             quoteCsv(QStringLiteral("说明"))}
                     .join(QLatin1Char(',')));
    for (const HighRow &row : report.rows) {
        lines.append(QStringList{quoteCsv(row.relativePath), quoteCsv(highRowKindText(row.kind)),
                                 quoteCsv(row.md5A), quoteCsv(row.md5B), quoteCsv(row.note)}
                         .join(QLatin1Char(',')));
    }
    const QByteArray payload =
        QByteArray("\xEF\xBB\xBF") + lines.join(QLatin1Char('\n')).toUtf8() + QByteArray("\n");

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

QString highCsvInsideTreeReason(const QString &dirA, const QString &dirB, const QString &csvPath)
{
    const QString trimmed = csvPath.trimmed();
    if (trimmed.isEmpty())
        return QStringLiteral("请选择要导出的文件");
    const QFileInfo info(trimmed);
    if (info.exists() && info.isDir())
        return QStringLiteral("导出路径是一个目录");

    const QString anchor = canonicalAnchor(trimmed);
    if (anchor.isEmpty())
        return QStringLiteral("无法解析导出路径");

    const auto blocked = [&](const QString &dir) {
        const QString root = QFileInfo(dir.trimmed()).canonicalFilePath();
        return !root.isEmpty() && withinRoot(root, anchor);
    };
    if (blocked(dirA) || blocked(dirB))
        return QStringLiteral("不能把导出文件保存到被比较的目录中");
    return {};
}

} // namespace tw
