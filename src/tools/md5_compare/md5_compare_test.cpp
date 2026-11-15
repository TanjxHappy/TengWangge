#include "core/data_paths.h"
#include "core/platform_services.h"
#include "tools/md5_compare/compare_engine.h"
#include "tools/md5_compare/md5_compare_tool.h"

#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>

#include <functional>

#include <sys/stat.h>

namespace {

bool writeFile(const QString &path, const QByteArray &data)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(data) == data.size();
}

const tw::CompareEntry *findEntry(const tw::CompareResult &result, const QString &path)
{
    for (const tw::CompareEntry &entry : result.entries) {
        if (entry.relativePath == path)
            return &entry;
    }
    return nullptr;
}

bool hasProblem(const tw::CompareResult &result, const QString &path, const QString &fragment)
{
    for (const tw::CompareProblem &problem : result.problems) {
        if (problem.relativePath == path && problem.message.contains(fragment))
            return true;
    }
    return false;
}

void collectTree(const QString &root, const QString &dir, QStringList *out)
{
    const QFileInfoList infos = QDir(dir).entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &info : infos) {
        QString content;
        if (info.isFile() && !info.isSymLink()) {
            QFile file(info.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly))
                content = QString::fromLatin1(file.readAll().toHex());
        }
        out->append(QDir(root).relativeFilePath(info.absoluteFilePath()) + QLatin1Char('|') + content
                    + QLatin1Char('|') + (info.isSymLink() ? info.symLinkTarget() : QString()));
        if (info.isDir() && !info.isSymLink())
            collectTree(root, info.absoluteFilePath(), out);
    }
}

QStringList snapshotTree(const QString &root)
{
    QStringList out;
    collectTree(root, root, &out);
    out.sort();
    return out;
}

class ModeGuard
{
public:
    ModeGuard(QString path, mode_t mode)
        : m_path(std::move(path))
        , m_mode(mode)
    {
    }
    ~ModeGuard()
    {
        if (!m_path.isEmpty())
            ::chmod(QFile::encodeName(m_path).constData(), m_mode);
    }

private:
    QString m_path;
    mode_t m_mode;
};

tw::ToolContext contextFor(tw::PlatformServices &platform, const tw::Md5CompareTool &tool)
{
    tw::ToolContext context;
    context.info = tool.info();
    context.services = platform.servicesFor(tool.info().id);
    return context;
}

} // namespace

class Md5CompareTests : public QObject
{
    Q_OBJECT

private slots:
    void info_matchesContract();
    void initialize_doesNotCreateWidgets();
    void initialize_failsWhenDataDirRejected();

    void compare_sameContent();
    void compare_differentContent();
    void compare_onlyOneSide();
    void compare_emptyDirectories();
    void compare_hiddenNestedAndKnownMd5();
    void compare_symlinkEscapeIsNotFollowed();
    void compare_symlinkLoopIsBounded();
    void compare_internalSymlinksAreFollowed();
    void compare_specialFileIsReported();
    void compare_unreadableFileIsReported();
    void compare_unreadableDirectoryIsReported();
    void compare_cancelDropsPartialResults();
    void compare_hashCancelStopsMidFile();
    void compare_depthAndFileLimitsAreReported();
    void compare_doesNotWriteComparedDirectories();
    void compare_rejectsMissingOrNonDirectory();
    void report_csvAndBlockedPaths();
    void filter_matchesStatusAndPath();

    void ui_compareShowsRowsAndReleasesLock();
    void ui_layoutStaysSeparatedWhenNarrow();
    void ui_shutdownWhileRunningReleasesLock();
    void ui_remembersPathsOnlyInDataDir();
};

void Md5CompareTests::info_matchesContract()
{
    tw::Md5CompareTool tool;
    const tw::ToolInfo info = tool.info();
    QCOMPARE(info.id, QStringLiteral("md5-compare"));
    QCOMPARE(info.title, QStringLiteral("目录 MD5 比较"));
    QVERIFY(!info.summary.isEmpty());
    QCOMPARE(info.iconKey, QStringLiteral("status"));
    QVERIFY(tw::isValidIdentifier(info.id));
}

void Md5CompareTests::initialize_doesNotCreateWidgets()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    tw::PlatformServices platform(tw::DataPaths(temp.filePath(QStringLiteral("data"))));
    tw::Md5CompareTool tool;
    const int before = QApplication::allWidgets().size();
    QString error;
    QVERIFY2(tool.initialize(contextFor(platform, tool), &error), qPrintable(error));
    QCOMPARE(QApplication::allWidgets().size(), before);
    QCOMPARE(error, QString());
}

void Md5CompareTests::initialize_failsWhenDataDirRejected()
{
    tw::PlatformServices platform(tw::DataPaths(QStringLiteral("/etc/tengwangge-md5")));
    tw::Md5CompareTool tool;
    const int before = QApplication::allWidgets().size();
    QString error;
    QVERIFY(!tool.initialize(contextFor(platform, tool), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(QApplication::allWidgets().size(), before);
}

void Md5CompareTests::compare_sameContent()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    QVERIFY(writeFile(left.filePath(QStringLiteral("a.txt")), "same"));
    QVERIFY(writeFile(right.filePath(QStringLiteral("a.txt")), "same"));
    QVERIFY(writeFile(left.filePath(QStringLiteral("empty.dat")), QByteArray()));
    QVERIFY(writeFile(right.filePath(QStringLiteral("empty.dat")), QByteArray()));

    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error), qPrintable(error));
    QVERIFY(!result.cancelled);
    QCOMPARE(result.sameCount, 2);
    QCOMPARE(result.differentCount, 0);
    QCOMPARE(result.onlyInACount, 0);
    QCOMPARE(result.onlyInBCount, 0);
    QCOMPARE(result.problemCount, 0);

    const tw::CompareEntry *file = findEntry(result, QStringLiteral("a.txt"));
    QVERIFY(file);
    QCOMPARE(file->status, tw::CompareStatus::Same);
    QCOMPARE(file->md5A, file->md5B);
    QCOMPARE(file->md5A.size(), 32);

    const tw::CompareEntry *empty = findEntry(result, QStringLiteral("empty.dat"));
    QVERIFY(empty);
    QCOMPARE(empty->md5A, QStringLiteral("d41d8cd98f00b204e9800998ecf8427e"));
}

void Md5CompareTests::compare_differentContent()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    QVERIFY(writeFile(left.filePath(QStringLiteral("a.txt")), "aaa"));
    QVERIFY(writeFile(right.filePath(QStringLiteral("a.txt")), "bbb"));

    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error), qPrintable(error));
    QCOMPARE(result.differentCount, 1);
    QCOMPARE(result.sameCount, 0);
    const tw::CompareEntry *file = findEntry(result, QStringLiteral("a.txt"));
    QVERIFY(file);
    QCOMPARE(file->status, tw::CompareStatus::Different);
    QVERIFY(file->md5A != file->md5B);
    QVERIFY(!file->md5A.isEmpty());
    QVERIFY(!file->md5B.isEmpty());
}

void Md5CompareTests::compare_onlyOneSide()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    QVERIFY(writeFile(left.filePath(QStringLiteral("only-a.txt")), "a"));
    QVERIFY(writeFile(right.filePath(QStringLiteral("only-b.txt")), "b"));

    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error), qPrintable(error));
    QCOMPARE(result.onlyInACount, 1);
    QCOMPARE(result.onlyInBCount, 1);
    QCOMPARE(result.sameCount, 0);
    QCOMPARE(result.differentCount, 0);

    const tw::CompareEntry *onlyA = findEntry(result, QStringLiteral("only-a.txt"));
    const tw::CompareEntry *onlyB = findEntry(result, QStringLiteral("only-b.txt"));
    QVERIFY(onlyA && onlyB);
    QCOMPARE(onlyA->status, tw::CompareStatus::OnlyInA);
    QVERIFY(!onlyA->md5A.isEmpty());
    QVERIFY(onlyA->md5B.isEmpty());
    QCOMPARE(onlyB->status, tw::CompareStatus::OnlyInB);
    QVERIFY(onlyB->md5A.isEmpty());
    QVERIFY(!onlyB->md5B.isEmpty());
}

void Md5CompareTests::compare_emptyDirectories()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    QVERIFY(QDir().mkpath(left.filePath(QStringLiteral("empty/nested"))));
    QVERIFY(QDir().mkpath(right.filePath(QStringLiteral("empty/nested"))));

    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error), qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(!result.cancelled);
    QVERIFY(result.entries.isEmpty());
    QVERIFY(result.problems.isEmpty());
    QCOMPARE(result.sameCount, 0);
    QCOMPARE(result.differentCount, 0);
    QCOMPARE(result.onlyInACount, 0);
    QCOMPARE(result.onlyInBCount, 0);
    QCOMPARE(result.problemCount, 0);
}

void Md5CompareTests::compare_hiddenNestedAndKnownMd5()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    const QByteArray abc("abc");
    const QByteArray binary("a\0b", 3);
    QVERIFY(writeFile(left.filePath(QStringLiteral(".secret")), abc));
    QVERIFY(writeFile(right.filePath(QStringLiteral(".secret")), abc));
    QVERIFY(writeFile(left.filePath(QStringLiteral("子目录/文件.txt")), binary));
    QVERIFY(writeFile(right.filePath(QStringLiteral("子目录/文件.txt")), binary));

    tw::CompareResult result;
    QString error;
    int last = -1;
    bool sawHash = false;
    bool progressOk = true;
    QVERIFY2(tw::compareDirectories(
                 left.path(), right.path(), &result, &error,
                 [&](const tw::CompareProgress &progress) {
                     if (progress.enumerating)
                         return;
                     sawHash = true;
                     if (progress.completed < last || progress.total < progress.completed)
                         progressOk = false;
                     last = progress.completed;
                 }),
             qPrintable(error));
    QVERIFY(progressOk);
    QVERIFY(sawHash);

    const tw::CompareEntry *secret = findEntry(result, QStringLiteral(".secret"));
    QVERIFY(secret);
    QCOMPARE(secret->status, tw::CompareStatus::Same);
    QCOMPARE(secret->md5A, QStringLiteral("900150983cd24fb0d6963f7d28e17f72"));

    const tw::CompareEntry *nested = findEntry(result, QStringLiteral("子目录/文件.txt"));
    QVERIFY(nested);
    QCOMPARE(nested->status, tw::CompareStatus::Same);
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(binary);
    QCOMPARE(nested->md5A, QString::fromLatin1(hash.result().toHex()));
    QVERIFY(!nested->relativePath.contains(QLatin1Char('\\')));
    QVERIFY(!nested->relativePath.startsWith(QLatin1Char('/')));

    for (int i = 1; i < result.entries.size(); ++i)
        QVERIFY(result.entries.at(i - 1).relativePath <= result.entries.at(i).relativePath);
}

void Md5CompareTests::compare_symlinkEscapeIsNotFollowed()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QTemporaryDir outside;
    QVERIFY(left.isValid() && right.isValid() && outside.isValid());
    const QByteArray secret("top-secret-content");
    QVERIFY(writeFile(outside.filePath(QStringLiteral("secret.txt")), secret));
    QVERIFY(QFile::link(outside.filePath(QStringLiteral("secret.txt")), left.filePath(QStringLiteral("leak"))));
    QVERIFY(QFile::link(outside.path(), left.filePath(QStringLiteral("leakdir"))));
    QVERIFY(QFile::link(QStringLiteral(".."), left.filePath(QStringLiteral("up"))));

    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(secret);
    const QString secretMd5 = QString::fromLatin1(hash.result().toHex());

    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error), qPrintable(error));
    QVERIFY(hasProblem(result, QStringLiteral("leak"), QStringLiteral("之外")));
    QVERIFY(hasProblem(result, QStringLiteral("leakdir"), QStringLiteral("之外")));
    QVERIFY(hasProblem(result, QStringLiteral("up"), QStringLiteral("之外")));
    for (const tw::CompareEntry &entry : result.entries) {
        QVERIFY(entry.md5A != secretMd5);
        QVERIFY(entry.md5B != secretMd5);
        QVERIFY(!entry.relativePath.startsWith(QStringLiteral("leakdir/")));
        QVERIFY(!entry.relativePath.startsWith(QStringLiteral("up/")));
        QCOMPARE(entry.status, tw::CompareStatus::Problem);
    }
    QCOMPARE(result.onlyInACount, 0);
}

void Md5CompareTests::compare_symlinkLoopIsBounded()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    QVERIFY(writeFile(left.filePath(QStringLiteral("sub/f.txt")), "abc"));
    QVERIFY(writeFile(right.filePath(QStringLiteral("sub/f.txt")), "abc"));
    QVERIFY(QFile::link(left.filePath(QStringLiteral("sub")), left.filePath(QStringLiteral("sub/loop"))));
    QVERIFY(QFile::link(right.filePath(QStringLiteral("sub")), right.filePath(QStringLiteral("sub/loop"))));
    QVERIFY(QFile::link(left.filePath(QStringLiteral("b")), left.filePath(QStringLiteral("a"))));
    QVERIFY(QFile::link(left.filePath(QStringLiteral("a")), left.filePath(QStringLiteral("b"))));

    QElapsedTimer timer;
    timer.start();
    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error), qPrintable(error));
    QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));

    const tw::CompareEntry *file = findEntry(result, QStringLiteral("sub/f.txt"));
    QVERIFY(file);
    QCOMPARE(file->status, tw::CompareStatus::Same);
    QVERIFY(findEntry(result, QStringLiteral("sub/loop/f.txt")) == nullptr);
    QVERIFY(hasProblem(result, QStringLiteral("sub/loop"), QStringLiteral("循环")));
    QVERIFY(hasProblem(result, QStringLiteral("a"), QStringLiteral("循环")));
    QVERIFY(hasProblem(result, QStringLiteral("b"), QStringLiteral("循环")));
    QVERIFY(findEntry(result, QStringLiteral("a/f.txt")) == nullptr);
    QVERIFY(findEntry(result, QStringLiteral("b/f.txt")) == nullptr);
}

void Md5CompareTests::compare_internalSymlinksAreFollowed()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    QVERIFY(writeFile(left.filePath(QStringLiteral("sub/f.txt")), "abc"));
    QVERIFY(writeFile(right.filePath(QStringLiteral("sub/f.txt")), "abc"));
    QVERIFY(QFile::link(QStringLiteral("sub/f.txt"), left.filePath(QStringLiteral("link.txt"))));
    QVERIFY(QFile::link(QStringLiteral("sub/f.txt"), right.filePath(QStringLiteral("link.txt"))));
    QVERIFY(QFile::link(left.filePath(QStringLiteral("sub")), left.filePath(QStringLiteral("alias"))));
    QVERIFY(QFile::link(right.filePath(QStringLiteral("sub")), right.filePath(QStringLiteral("alias"))));

    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error), qPrintable(error));
    const tw::CompareEntry *real = findEntry(result, QStringLiteral("sub/f.txt"));
    const tw::CompareEntry *link = findEntry(result, QStringLiteral("link.txt"));
    const tw::CompareEntry *alias = findEntry(result, QStringLiteral("alias/f.txt"));
    QVERIFY(real && link && alias);
    QCOMPARE(real->status, tw::CompareStatus::Same);
    QCOMPARE(link->status, tw::CompareStatus::Same);
    QCOMPARE(alias->status, tw::CompareStatus::Same);
    QCOMPARE(link->md5A, real->md5A);
    QCOMPARE(alias->md5A, real->md5A);
    QCOMPARE(real->md5A, QStringLiteral("900150983cd24fb0d6963f7d28e17f72"));
}

void Md5CompareTests::compare_specialFileIsReported()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    const QByteArray encoded = QFile::encodeName(left.filePath(QStringLiteral("pipe")));
    QCOMPARE(::mkfifo(encoded.constData(), 0644), 0);

    QElapsedTimer timer;
    timer.start();
    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error), qPrintable(error));
    QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(hasProblem(result, QStringLiteral("pipe"), QStringLiteral("不是普通文件")));
    const tw::CompareEntry *pipe = findEntry(result, QStringLiteral("pipe"));
    QVERIFY(pipe);
    QCOMPARE(pipe->status, tw::CompareStatus::Problem);
    QVERIFY(pipe->md5A.isEmpty());
    QCOMPARE(result.onlyInACount, 0);
}

void Md5CompareTests::compare_unreadableFileIsReported()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    const QString path = left.filePath(QStringLiteral("locked.txt"));
    QVERIFY(writeFile(path, "secret"));
    ModeGuard guard(path, 0644);
    QCOMPARE(::chmod(QFile::encodeName(path).constData(), 0), 0);

    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error), qPrintable(error));
    QVERIFY(hasProblem(result, QStringLiteral("locked.txt"), QStringLiteral("无法读取")));
    const tw::CompareEntry *locked = findEntry(result, QStringLiteral("locked.txt"));
    QVERIFY(locked);
    QCOMPARE(locked->status, tw::CompareStatus::Problem);
    QVERIFY(locked->md5A.isEmpty());
    QCOMPARE(result.onlyInACount, 0);
}

void Md5CompareTests::compare_unreadableDirectoryIsReported()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    const QString dir = left.filePath(QStringLiteral("secret"));
    QVERIFY(writeFile(dir + QStringLiteral("/file.txt"), "secret"));
    ModeGuard guard(dir, 0755);
    QCOMPARE(::chmod(QFile::encodeName(dir).constData(), 0), 0);

    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error), qPrintable(error));
    QVERIFY(hasProblem(result, QStringLiteral("secret"), QStringLiteral("无法读取")));
    QVERIFY(findEntry(result, QStringLiteral("secret/file.txt")) == nullptr);

    QTemporaryDir lockedRoot;
    QVERIFY(lockedRoot.isValid());
    ModeGuard rootGuard(lockedRoot.path(), 0755);
    QCOMPARE(::chmod(QFile::encodeName(lockedRoot.path()).constData(), 0), 0);
    error.clear();
    QVERIFY(!tw::compareDirectories(lockedRoot.path(), right.path(), &result, &error));
    QVERIFY(error.contains(QStringLiteral("无法")));
}

void Md5CompareTests::compare_cancelDropsPartialResults()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    QVERIFY(writeFile(left.filePath(QStringLiteral("a.txt")), "abc"));
    QVERIFY(writeFile(right.filePath(QStringLiteral("a.txt")), "abc"));

    tw::CompareResult already;
    QString error;
    QVERIFY(tw::compareDirectories(left.path(), right.path(), &already, &error, {}, [] { return true; }));
    QVERIFY(already.cancelled);
    QVERIFY(already.entries.isEmpty());
    QVERIFY(error.isEmpty());

    bool stop = false;
    tw::CompareResult mid;
    QVERIFY(tw::compareDirectories(
        left.path(), right.path(), &mid, &error, [&](const tw::CompareProgress &) { stop = true; },
        [&] { return stop; }));
    QVERIFY(mid.cancelled);
    QVERIFY(mid.entries.isEmpty());
    QCOMPARE(mid.sameCount, 0);
}

void Md5CompareTests::compare_hashCancelStopsMidFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("big.bin"));
    QVERIFY(writeFile(path, QByteArray(3 * 256 * 1024, 'x')));
    int calls = 0;
    QString hex;
    QString error;
    QElapsedTimer timer;
    timer.start();
    const tw::FileHashStatus status = tw::hashFileMd5(
        path, QFileInfo(dir.path()).canonicalFilePath(), &hex, &error,
        [&] { return ++calls >= 3; });
    QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(status, tw::FileHashStatus::Cancelled);
    QVERIFY(hex.isEmpty());
    QVERIFY(calls >= 3);

    QString outsideHex;
    QString outsideError;
    QCOMPARE(tw::hashFileMd5(QStringLiteral("/etc/hostname"), QFileInfo(dir.path()).canonicalFilePath(),
                             &outsideHex, &outsideError),
             tw::FileHashStatus::Failed);
    QVERIFY(outsideHex.isEmpty());
    QVERIFY(!outsideError.isEmpty());
}

void Md5CompareTests::compare_depthAndFileLimitsAreReported()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    QVERIFY(writeFile(left.filePath(QStringLiteral("root.txt")), "r"));
    QVERIFY(writeFile(left.filePath(QStringLiteral("sub/file.txt")), "s"));
    QVERIFY(writeFile(left.filePath(QStringLiteral("sub/deep/x.txt")), "d"));

    tw::CompareLimits limits;
    limits.maxDepth = 2;
    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error, {}, {}, limits),
             qPrintable(error));
    QVERIFY(findEntry(result, QStringLiteral("root.txt")));
    QVERIFY(findEntry(result, QStringLiteral("sub/file.txt")));
    QVERIFY(findEntry(result, QStringLiteral("sub/deep/x.txt")) == nullptr);
    QVERIFY(hasProblem(result, QStringLiteral("sub/deep"), QStringLiteral("层级")));

    QVERIFY(writeFile(left.filePath(QStringLiteral("a.txt")), "a"));
    QVERIFY(writeFile(left.filePath(QStringLiteral("b.txt")), "b"));
    QVERIFY(writeFile(left.filePath(QStringLiteral("c.txt")), "c"));
    limits = {};
    limits.maxFiles = 1;
    tw::CompareResult capped;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &capped, &error, {}, {}, limits),
             qPrintable(error));
    QVERIFY(capped.truncated);
    QVERIFY(findEntry(capped, QStringLiteral("a.txt")));
    QVERIFY(findEntry(capped, QStringLiteral("b.txt")) == nullptr);
    QVERIFY(findEntry(capped, QStringLiteral("c.txt")) == nullptr);
    bool sawLimit = false;
    for (const tw::CompareProblem &problem : capped.problems) {
        if (problem.message.contains(QStringLiteral("上限")))
            sawLimit = true;
    }
    QVERIFY(sawLimit);
}

void Md5CompareTests::compare_doesNotWriteComparedDirectories()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QVERIFY(left.isValid() && right.isValid());
    QVERIFY(writeFile(left.filePath(QStringLiteral("keep.txt")), "keep"));
    QVERIFY(writeFile(right.filePath(QStringLiteral("sub/keep.txt")), "keep"));
    QVERIFY(QFile::link(left.filePath(QStringLiteral("keep.txt")), left.filePath(QStringLiteral("link"))));
    const QStringList beforeLeft = snapshotTree(left.path());
    const QStringList beforeRight = snapshotTree(right.path());

    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error), qPrintable(error));
    QCOMPARE(snapshotTree(left.path()), beforeLeft);
    QCOMPARE(snapshotTree(right.path()), beforeRight);
}

void Md5CompareTests::compare_rejectsMissingOrNonDirectory()
{
    QTemporaryDir right;
    QVERIFY(right.isValid());
    tw::CompareResult result;
    QString error;
    QVERIFY(!tw::compareDirectories(QStringLiteral("/no/such/md5-dir"), right.path(), &result, &error));
    QVERIFY(error.contains(QStringLiteral("不存在")));

    const QString file = right.filePath(QStringLiteral("file.txt"));
    QVERIFY(writeFile(file, "x"));
    error.clear();
    QVERIFY(!tw::compareDirectories(file, right.path(), &result, &error));
    QVERIFY(error.contains(QStringLiteral("不是目录")));
}

void Md5CompareTests::report_csvAndBlockedPaths()
{
    QTemporaryDir left;
    QTemporaryDir right;
    QTemporaryDir out;
    QVERIFY(left.isValid() && right.isValid() && out.isValid());
    const QString oddName = QStringLiteral("a\"b,c.txt");
    QVERIFY(writeFile(left.filePath(oddName), "abc"));
    QVERIFY(writeFile(right.filePath(oddName), "abc"));
    QVERIFY(writeFile(left.filePath(QStringLiteral("文件.txt")), "x"));

    tw::CompareResult result;
    QString error;
    QVERIFY2(tw::compareDirectories(left.path(), right.path(), &result, &error), qPrintable(error));
    const QString report = out.filePath(QStringLiteral("报告.csv"));
    QVERIFY2(tw::writeCompareReport(report, result, &error), qPrintable(error));

    QFile file(report);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    QVERIFY(bytes.startsWith("\xEF\xBB\xBF"));
    const QString text = QString::fromUtf8(bytes.mid(3));
    QVERIFY(text.contains(QStringLiteral("\"相对路径\",\"状态\",\"目录A的MD5\",\"目录B的MD5\",\"说明\"")));
    QVERIFY(text.contains(QStringLiteral("\"a\"\"b,c.txt\"")));
    QVERIFY(text.contains(QStringLiteral("\"文件.txt\"")));
    QVERIFY(text.contains(QStringLiteral("\"相同\"")));
    QVERIFY(text.contains(QStringLiteral("900150983cd24fb0d6963f7d28e17f72")));

    QVERIFY(!tw::writeCompareReport(out.filePath(QStringLiteral("missing/报告.csv")), result, &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(!tw::compareReportBlockReason(left.path(), right.path(), left.filePath(QStringLiteral("out.csv")))
                 .isEmpty());
    QVERIFY(tw::compareReportBlockReason(left.path(), right.path(), report).isEmpty());
    const QString sibling = left.path() + QStringLiteral("-extra");
    QVERIFY(QDir().mkpath(sibling));
    QVERIFY(tw::compareReportBlockReason(left.path(), right.path(),
                                         QDir(sibling).filePath(QStringLiteral("out.csv")))
                .isEmpty());
    QVERIFY(QFile::link(left.path(), out.filePath(QStringLiteral("alias"))));
    QVERIFY(!tw::compareReportBlockReason(left.path(), right.path(),
                                          out.filePath(QStringLiteral("alias/out.csv")))
                 .isEmpty());
}

void Md5CompareTests::filter_matchesStatusAndPath()
{
    tw::CompareEntry entry;
    entry.relativePath = QStringLiteral("Dir/Foo.txt");
    entry.status = tw::CompareStatus::Same;
    QVERIFY(tw::compareEntryMatches(entry, -1, QString()));
    QVERIFY(tw::compareEntryMatches(entry, static_cast<int>(tw::CompareStatus::Same), QStringLiteral("  foo ")));
    QVERIFY(!tw::compareEntryMatches(entry, static_cast<int>(tw::CompareStatus::Different), QStringLiteral("foo")));
    QVERIFY(!tw::compareEntryMatches(entry, -1, QStringLiteral("bar")));

    tw::CompareEntry problem;
    problem.relativePath = QStringLiteral("bad");
    problem.status = tw::CompareStatus::Problem;
    QVERIFY(tw::compareEntryMatches(problem, static_cast<int>(tw::CompareStatus::Problem), QString()));
    QVERIFY(!tw::compareEntryMatches(problem, static_cast<int>(tw::CompareStatus::OnlyInA), QString()));
}

void Md5CompareTests::ui_compareShowsRowsAndReleasesLock()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString dirA = temp.filePath(QStringLiteral("a"));
    const QString dirB = temp.filePath(QStringLiteral("b"));
    QVERIFY(writeFile(dirA + QStringLiteral("/same.txt"), "same"));
    QVERIFY(writeFile(dirB + QStringLiteral("/same.txt"), "same"));
    QVERIFY(writeFile(dirA + QStringLiteral("/diff.txt"), "aaa"));
    QVERIFY(writeFile(dirB + QStringLiteral("/diff.txt"), "bbb"));
    QVERIFY(writeFile(dirA + QStringLiteral("/onlyA.txt"), "a"));
    QVERIFY(writeFile(dirB + QStringLiteral("/onlyB.txt"), "b"));

    tw::PlatformServices platform(tw::DataPaths(temp.filePath(QStringLiteral("data"))));
    tw::Md5CompareTool tool;
    QString error;
    QVERIFY2(tool.initialize(contextFor(platform, tool), &error), qPrintable(error));
    const QVector<tw::ToolPage> pages = tool.createPages();
    QCOMPARE(pages.size(), 1);
    QCOMPARE(pages.at(0).info.id, QStringLiteral("compare"));
    QCOMPARE(pages.at(0).info.title, QStringLiteral("比较"));
    std::unique_ptr<QWidget> owner(pages.at(0).widget);
    tw::Md5ComparePage *page = tool.page();
    QVERIFY(page);
    page->resize(1100, 760);
    page->show();
    QVERIFY(QTest::qWaitForWindowExposed(page));

    auto *editA = page->findChild<QLineEdit *>(QStringLiteral("md5DirA"));
    auto *editB = page->findChild<QLineEdit *>(QStringLiteral("md5DirB"));
    auto *start = page->findChild<QPushButton *>(QStringLiteral("md5Start"));
    auto *table = page->findChild<QTableView *>(QStringLiteral("md5Table"));
    auto *filter = page->findChild<QComboBox *>(QStringLiteral("md5Filter"));
    auto *search = page->findChild<QLineEdit *>(QStringLiteral("md5Search"));
    auto *counts = page->findChild<QLabel *>(QStringLiteral("md5Counts"));
    auto *exportButton = page->findChild<QPushButton *>(QStringLiteral("md5Export"));
    QVERIFY(editA && editB && start && table && filter && search && counts && exportButton);
    editA->setText(dirA);
    editB->setText(dirB);

    QVERIFY(!platform.gate()->isLocked());
    QVERIFY(!page->isBusy());
    start->click();
    QVERIFY(page->isBusy());
    QVERIFY(platform.gate()->isLocked());
    QVERIFY(platform.gate()->reason().contains(QStringLiteral("正在比较目录 MD5")));
    QTRY_VERIFY_WITH_TIMEOUT(!page->isBusy(), 5000);
    QVERIFY(!platform.gate()->isLocked());
    QVERIFY(exportButton->isEnabled());
    QCOMPARE(table->model()->rowCount(), 4);
    QVERIFY(counts->text().contains(QStringLiteral("相同 1")));
    QVERIFY(counts->text().contains(QStringLiteral("内容不同 1")));
    QVERIFY(counts->text().contains(QStringLiteral("仅在目录 A 1")));
    QVERIFY(counts->text().contains(QStringLiteral("仅在目录 B 1")));
    QVERIFY(counts->text().contains(QStringLiteral("问题 0")));
    QCOMPARE(platform.status()->current(tool.info().id).state, tw::StatusState::Success);

    filter->setCurrentIndex(filter->findText(QStringLiteral("仅在目录 A")));
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->index(0, 0).data(Qt::UserRole).toString(), QStringLiteral("onlyA.txt"));
    search->setText(QStringLiteral("no-such"));
    QCOMPARE(table->model()->rowCount(), 0);
    filter->setCurrentIndex(0);
    search->clear();
    QCOMPARE(table->model()->rowCount(), 4);
    search->setText(QStringLiteral("DIFF"));
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->index(0, 0).data(Qt::UserRole).toString(), QStringLiteral("diff.txt"));

    const QStringList beforeA = snapshotTree(dirA);
    const QStringList beforeB = snapshotTree(dirB);
    QCOMPARE(snapshotTree(dirA), beforeA);
    QCOMPARE(snapshotTree(dirB), beforeB);
}

void Md5CompareTests::ui_layoutStaysSeparatedWhenNarrow()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    tw::PlatformServices platform(tw::DataPaths(temp.filePath(QStringLiteral("data"))));
    tw::Md5CompareTool tool;
    QString error;
    QVERIFY2(tool.initialize(contextFor(platform, tool), &error), qPrintable(error));
    const auto pages = tool.createPages();
    std::unique_ptr<QWidget> owner(pages.at(0).widget);
    QWidget *page = tool.page();
    page->show();
    QVERIFY(QTest::qWaitForWindowExposed(page));
    const QList<QSize> sizes = {QSize(520, 420), QSize(640, 480), QSize(1100, 720)};
    for (const QSize &size : sizes) {
        page->resize(size);
        QCoreApplication::processEvents();

        auto *dirA = page->findChild<QLineEdit *>(QStringLiteral("md5DirA"));
        auto *dirB = page->findChild<QLineEdit *>(QStringLiteral("md5DirB"));
        auto *browseA = page->findChild<QPushButton *>(QStringLiteral("md5BrowseA"));
        auto *browseB = page->findChild<QPushButton *>(QStringLiteral("md5BrowseB"));
        auto *start = page->findChild<QPushButton *>(QStringLiteral("md5Start"));
        QVERIFY(dirA && dirB && browseA && browseB && start);

        const auto box = [page](QWidget *widget) {
            return QRect(widget->mapTo(page, QPoint(0, 0)), widget->size());
        };
        const QRect a = box(dirA);
        const QRect b = box(dirB);
        const QRect buttonA = box(browseA);
        const QRect buttonB = box(browseB);
        const QRect startBox = box(start);
        const auto fmt = [](const QRect &rect) {
            return QStringLiteral("%1,%2 %3x%4").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height());
        };
        const QString dump = QStringLiteral("A=%1 browseA=%2 B=%3 browseB=%4 start=%5 page=%6")
                                 .arg(fmt(a), fmt(buttonA), fmt(b), fmt(buttonB), fmt(startBox))
                                 .arg(page->width());
        QVERIFY2(!a.intersects(buttonA), qPrintable(dump));
        QVERIFY2(!b.intersects(buttonB), qPrintable(dump));
        QVERIFY2(!a.intersects(b), qPrintable(dump));
        QVERIFY2(!startBox.intersects(a) && !startBox.intersects(b) && !startBox.intersects(buttonA)
                     && !startBox.intersects(buttonB),
                 qPrintable(dump));
        QVERIFY2(a.height() >= dirA->fontMetrics().height(), qPrintable(dump));
        QVERIFY2(a.width() >= dirA->fontMetrics().horizontalAdvance(QStringLiteral("目录")), qPrintable(dump));
        QVERIFY2(buttonA.width() >= browseA->fontMetrics().horizontalAdvance(browseA->text()), qPrintable(dump));
        QVERIFY2(buttonA.right() <= page->width() && buttonB.right() <= page->width()
                     && startBox.right() <= page->width(),
                 qPrintable(dump));
        QVERIFY2(startBox.top() >= b.bottom(), qPrintable(dump));
    }
}

void Md5CompareTests::ui_shutdownWhileRunningReleasesLock()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString dirA = temp.filePath(QStringLiteral("a"));
    const QString dirB = temp.filePath(QStringLiteral("b"));
    QVERIFY(writeFile(dirA + QStringLiteral("/big.bin"), QByteArray(1024 * 1024, 'z')));
    QVERIFY(writeFile(dirB + QStringLiteral("/big.bin"), QByteArray(1024 * 1024, 'y')));

    tw::PlatformServices platform(tw::DataPaths(temp.filePath(QStringLiteral("data"))));
    tw::Md5CompareTool tool;
    QString error;
    QVERIFY2(tool.initialize(contextFor(platform, tool), &error), qPrintable(error));
    const auto pages = tool.createPages();
    std::unique_ptr<QWidget> owner(pages.at(0).widget);
    tw::Md5ComparePage *page = tool.page();
    page->findChild<QLineEdit *>(QStringLiteral("md5DirA"))->setText(dirA);
    page->findChild<QLineEdit *>(QStringLiteral("md5DirB"))->setText(dirB);
    page->findChild<QPushButton *>(QStringLiteral("md5Start"))->click();
    QVERIFY(page->isBusy());
    QVERIFY(platform.gate()->isLocked());
    tool.shutdown();
    QVERIFY(!page->isBusy());
    QVERIFY(!platform.gate()->isLocked());
    QCoreApplication::processEvents();
    QVERIFY(!platform.gate()->isLocked());
    tool.shutdown();
}

void Md5CompareTests::ui_remembersPathsOnlyInDataDir()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString dirA = temp.filePath(QStringLiteral("目录A"));
    const QString dirB = temp.filePath(QStringLiteral("目录B"));
    QVERIFY(QDir().mkpath(dirA));
    QVERIFY(QDir().mkpath(dirB));

    tw::PlatformServices platform(tw::DataPaths(temp.filePath(QStringLiteral("data"))));
    tw::Md5CompareTool tool;
    QString error;
    QVERIFY2(tool.initialize(contextFor(platform, tool), &error), qPrintable(error));
    const auto pages = tool.createPages();
    std::unique_ptr<QWidget> owner(pages.at(0).widget);
    auto *editA = tool.page()->findChild<QLineEdit *>(QStringLiteral("md5DirA"));
    auto *editB = tool.page()->findChild<QLineEdit *>(QStringLiteral("md5DirB"));
    editA->setText(dirA);
    editB->setText(dirB);
    emit editA->editingFinished();
    emit editB->editingFinished();

    const QString state = QDir(platform.paths().root()).filePath(QStringLiteral("tools/md5-compare/state.ini"));
    QVERIFY(QFileInfo::exists(state));
    QVERIFY(!QFileInfo::exists(QDir(dirA).filePath(QStringLiteral("state.ini"))));
    QVERIFY(!QFileInfo::exists(QDir(dirB).filePath(QStringLiteral("state.ini"))));
    QVERIFY(!state.startsWith(QDir(dirA).absolutePath()));

    owner.reset();
    tw::Md5ComparePage again(platform.servicesFor(tool.info().id));
    QCOMPARE(again.findChild<QLineEdit *>(QStringLiteral("md5DirA"))->text(), dirA);
    QCOMPARE(again.findChild<QLineEdit *>(QStringLiteral("md5DirB"))->text(), dirB);
}

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    QApplication app(argc, argv);
    Md5CompareTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "md5_compare_test.moc"
