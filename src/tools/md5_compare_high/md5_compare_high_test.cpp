#include "core/data_paths.h"
#include "core/platform_services.h"
#include "core/tool_info.h"
#include "shell/elided_label.h"
#include "shell/main_window.h"
#include "shell/status_strip.h"
#include "shell/theme.h"
#include "tools/md5_compare_high/md5_compare_high_tool.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QTreeView>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <memory>

namespace {
const QString kEmptyMd5 = QStringLiteral("d41d8cd98f00b204e9800998ecf8427e");
const QString kAbcMd5 = QStringLiteral("900150983cd24fb0d6963f7d28e17f72");

bool writeBytes(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

QStringList snapshotTree(const QString &root)
{
    QStringList names;
    QDirIterator it(root,
                    QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        names.append(it.filePath());
    }
    names.sort();
    return names;
}

const tw::HighRow *findRow(const tw::HighReport &report, const QString &path)
{
    for (const tw::HighRow &row : report.rows) {
        if (row.relativePath == path)
            return &row;
    }
    return nullptr;
}

QString issueBlob(const tw::HighReport &report)
{
    QStringList parts;
    for (const tw::HighIssue &issue : report.issues)
        parts.append(issue.relativePath + QLatin1Char(' ') + issue.message);
    for (const tw::HighRow &row : report.rows)
        parts.append(row.relativePath + QLatin1Char(' ') + row.note + QLatin1Char(' ') + row.md5A
                     + QLatin1Char(' ') + row.md5B);
    return parts.join(QLatin1Char('\n'));
}

class ModeGuard
{
public:
    ModeGuard(const QString &path, mode_t mode)
        : m_path(path)
        , m_mode(mode)
    {
    }

    ~ModeGuard() { ::chmod(QFile::encodeName(m_path).constData(), m_mode); }

private:
    QString m_path;
    mode_t m_mode;
};

struct Harness
{
    QTemporaryDir dir;
    std::unique_ptr<tw::PlatformServices> platform;
    tw::Md5CompareHighTool tool;
    tw::Md5CompareHighPage *page = nullptr;

    bool open(QString *error)
    {
        if (!dir.isValid()) {
            if (error)
                *error = QStringLiteral("临时目录不可用");
            return false;
        }
        platform = std::make_unique<tw::PlatformServices>(tw::DataPaths(dir.filePath(QStringLiteral("data"))));
        tw::ToolContext context;
        context.info = tool.info();
        context.services = platform->servicesFor(context.info.id);
        if (!tool.initialize(context, error))
            return false;
        const QVector<tw::ToolPage> pages = tool.createPages();
        if (pages.size() != 1 || tool.page() == nullptr)
            return false;
        page = tool.page();
        return true;
    }

    ~Harness()
    {
        tool.shutdown();
        delete page;
        page = nullptr;
    }
};

QRect mappedRect(QWidget *page, QWidget *widget)
{
    return QRect(widget->mapTo(page, QPoint(0, 0)), widget->size());
}

QString layoutProblem(QWidget *page)
{
    const QStringList names{
        QStringLiteral("md5HighDirA"),     QStringLiteral("md5HighDirB"),
        QStringLiteral("md5HighBrowseA"),  QStringLiteral("md5HighBrowseB"),
        QStringLiteral("md5HighStart"),    QStringLiteral("md5HighCancel"),
        QStringLiteral("md5HighExport"),   QStringLiteral("md5HighFilter"),
        QStringLiteral("md5HighSearch"),   QStringLiteral("md5HighCounts"),
        QStringLiteral("md5HighTable"),    QStringLiteral("md5HighProblems"),
        QStringLiteral("md5HighState"),    QStringLiteral("md5HighTreeA"),
        QStringLiteral("md5HighTreeB"),    QStringLiteral("md5HighPreview"),
    };
    QVector<QWidget *> widgets;
    widgets.reserve(names.size());
    for (const QString &name : names) {
        auto *widget = page->findChild<QWidget *>(name);
        if (!widget)
            return QStringLiteral("缺少控件 ") + name;
        if (!widget->isVisible() || widget->width() <= 0 || widget->height() <= 0)
            return name + QStringLiteral(" 不可见");
        const QRect got = mappedRect(page, widget);
        if (!page->rect().contains(got))
            return name + QStringLiteral(" 超出页面");
        widgets.append(widget);
    }
    for (int i = 0; i < widgets.size(); ++i) {
        for (int j = i + 1; j < widgets.size(); ++j) {
            if (mappedRect(page, widgets.at(i)).intersects(mappedRect(page, widgets.at(j))))
                return widgets.at(i)->objectName() + QStringLiteral(" 与 ") + widgets.at(j)->objectName()
                    + QStringLiteral(" 重叠");
        }
    }

    auto *dirB = page->findChild<QLineEdit *>(QStringLiteral("md5HighDirB"));
    auto *start = page->findChild<QPushButton *>(QStringLiteral("md5HighStart"));
    if (mappedRect(page, start).top() < mappedRect(page, dirB).bottom())
        return QStringLiteral("开始比较没有位于目录 B 下方");

    const QStringList buttons{QStringLiteral("md5HighBrowseA"), QStringLiteral("md5HighBrowseB"),
                              QStringLiteral("md5HighStart"), QStringLiteral("md5HighCancel"),
                              QStringLiteral("md5HighExport")};
    for (const QString &name : buttons) {
        auto *button = page->findChild<QPushButton *>(name);
        const int needed = button->fontMetrics().horizontalAdvance(button->text());
        if (button->width() < needed)
            return name + QStringLiteral(" 宽度不足以显示文字");
    }

    auto *table = page->findChild<QTableView *>(QStringLiteral("md5HighTable"));
    if (table->height() < 40 || table->width() < 48)
        return QStringLiteral("结果表太小");
    auto *problems = page->findChild<QWidget *>(QStringLiteral("md5HighProblems"));
    if (problems->height() < 24)
        return QStringLiteral("问题列表太小");
    auto *treeA = page->findChild<QTreeView *>(QStringLiteral("md5HighTreeA"));
    auto *treeB = page->findChild<QTreeView *>(QStringLiteral("md5HighTreeB"));
    if (treeA->height() < 40 || treeB->height() < 40)
        return QStringLiteral("目录树太小");
    auto *horizontal = page->findChild<QSplitter *>(QStringLiteral("md5HighSplitHorizontal"));
    auto *vertical = page->findChild<QSplitter *>(QStringLiteral("md5HighSplitVertical"));
    if (!horizontal || !vertical)
        return QStringLiteral("缺少可拖动分隔");
    if (horizontal->orientation() != Qt::Horizontal || horizontal->count() != 3)
        return QStringLiteral("横向分隔不是三块");
    if (vertical->orientation() != Qt::Vertical || vertical->count() != 2)
        return QStringLiteral("纵向分隔不是两块");
    if (page->width() >= 1100 && page->height() >= 700) {
        auto *toolbar = page->findChild<QWidget *>(QStringLiteral("md5HighToolbar"));
        if (!toolbar || toolbar->height() <= 0 || toolbar->height() >= treeA->height())
            return QStringLiteral("工具条挤占了结果区");
        if (treeA->height() < 120 || treeB->height() < 120 || table->height() < 120)
            return QStringLiteral("1180 宽度下结果区不完整");
    }
    return {};
}

QString labelText(const QLabel *label)
{
    if (const auto *elided = qobject_cast<const tw::ElidedLabel *>(label))
        return elided->fullText();
    return label->text();
}

QModelIndex findTreePath(QTreeView *view, const QString &path, const QModelIndex &parent = QModelIndex())
{
    const QAbstractItemModel *model = view->model();
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        if (index.data(Qt::UserRole).toString() == path)
            return index;
        const QModelIndex nested = findTreePath(view, path, index);
        if (nested.isValid())
            return nested;
    }
    return {};
}

} // namespace

class Md5CompareHighTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void info_idAndTitle();
    void initialize_createsNoWidgets();
    void initialize_rejectsSystemDataRoot();
    void digest_sameAndEmptyFile();
    void digest_differentContent();
    void digest_onlyOneSide();
    void digest_emptyDirectories();
    void digest_hiddenChineseAndKnownMd5();
    void digest_symlinkEscape();
    void digest_symlinkCycleAndInternalLink();
    void digest_specialFile();
    void digest_unreadableFile();
    void digest_unreadableDirectory();
    void digest_cancelDropsPartial();
    void digest_hashCancelAndOutsideFile();
    void digest_depthAndFileCaps();
    void digest_doesNotModifyTrees();
    void digest_rejectsMissingAndFile();
    void csv_bomQuotingAndBlockedPaths();
    void filter_statusAndPath();
    void ui_compareLockFilterAndExport();
    void ui_treesSyncDetailAndSort();
    void ui_layoutAtTypicalSizes();
    void ui_saveReferenceShots();
    void ui_mainWindowShowsTable();
    void ui_cancelReleasesLockAndDropsRows();
    void ui_failureReleasesLock();
    void ui_shutdownReleasesLock();
    void ui_memoryStaysInDataDir();
};

void Md5CompareHighTests::initTestCase()
{
    QVERIFY(::geteuid() != 0);
}

void Md5CompareHighTests::info_idAndTitle()
{
    tw::Md5CompareHighTool tool;
    const tw::ToolInfo info = tool.info();
    QCOMPARE(info.id, QStringLiteral("md5-compare-high"));
    QCOMPARE(info.title, QStringLiteral("目录 MD5 比较high"));
    QVERIFY(!info.id.contains(QLatin1Char('_')));
    QVERIFY(tw::isValidIdentifier(info.id));
    QVERIFY(!info.summary.isEmpty());
    QCOMPARE(info.iconKey, QStringLiteral("status"));
}

void Md5CompareHighTests::initialize_createsNoWidgets()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    tw::PlatformServices platform(tw::DataPaths(dir.filePath(QStringLiteral("data"))));
    tw::Md5CompareHighTool tool;
    tw::ToolContext context;
    context.info = tool.info();
    context.services = platform.servicesFor(context.info.id);
    const int before = QApplication::allWidgets().size();
    QString error;
    QVERIFY2(tool.initialize(context, &error), qPrintable(error));
    QCOMPARE(QApplication::allWidgets().size(), before);
    QVERIFY(tool.page() == nullptr);
}

void Md5CompareHighTests::initialize_rejectsSystemDataRoot()
{
    tw::PlatformServices platform(tw::DataPaths(QStringLiteral("/etc/tengwangge-md5-high")));
    tw::Md5CompareHighTool tool;
    tw::ToolContext context;
    context.info = tool.info();
    context.services = platform.servicesFor(context.info.id);
    QString error;
    QVERIFY(!tool.initialize(context, &error));
    QVERIFY(error.contains(QStringLiteral("系统目录")));
}

void Md5CompareHighTests::digest_sameAndEmptyFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(a + QStringLiteral("/empty.bin"), QByteArray()));
    QVERIFY(writeBytes(b + QStringLiteral("/empty.bin"), QByteArray()));
    QVERIFY(writeBytes(a + QStringLiteral("/same.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(b + QStringLiteral("/same.txt"), QByteArray("abc")));

    tw::HighReport report;
    QString error;
    QVERIFY(tw::digestDirectoryPair(a, b, &report, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(!report.cancelled);
    QCOMPARE(report.matchCount, 2);
    QCOMPARE(report.mismatchCount, 0);
    const tw::HighRow *empty = findRow(report, QStringLiteral("empty.bin"));
    const tw::HighRow *same = findRow(report, QStringLiteral("same.txt"));
    QVERIFY(empty && same);
    QCOMPARE(empty->kind, tw::HighRowKind::Match);
    QCOMPARE(empty->md5A, kEmptyMd5);
    QCOMPARE(empty->md5B, kEmptyMd5);
    QCOMPARE(same->md5A, kAbcMd5);
    QCOMPARE(same->md5B, kAbcMd5);
}

void Md5CompareHighTests::digest_differentContent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(a + QStringLiteral("/diff.txt"), QByteArray("one")));
    QVERIFY(writeBytes(b + QStringLiteral("/diff.txt"), QByteArray("two")));

    tw::HighReport report;
    QString error;
    QVERIFY(tw::digestDirectoryPair(a, b, &report, &error));
    QCOMPARE(report.mismatchCount, 1);
    const tw::HighRow *row = findRow(report, QStringLiteral("diff.txt"));
    QVERIFY(row);
    QCOMPARE(row->kind, tw::HighRowKind::Mismatch);
    QVERIFY(!row->md5A.isEmpty());
    QVERIFY(!row->md5B.isEmpty());
    QVERIFY(row->md5A != row->md5B);
}

void Md5CompareHighTests::digest_onlyOneSide()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(a + QStringLiteral("/onlyA.txt"), QByteArray("a")));
    QVERIFY(writeBytes(b + QStringLiteral("/onlyB.txt"), QByteArray("b")));

    tw::HighReport report;
    QString error;
    QVERIFY(tw::digestDirectoryPair(a, b, &report, &error));
    QCOMPARE(report.onlyACount, 1);
    QCOMPARE(report.onlyBCount, 1);
    const tw::HighRow *onlyA = findRow(report, QStringLiteral("onlyA.txt"));
    const tw::HighRow *onlyB = findRow(report, QStringLiteral("onlyB.txt"));
    QVERIFY(onlyA && onlyB);
    QCOMPARE(onlyA->kind, tw::HighRowKind::OnlyA);
    QVERIFY(!onlyA->md5A.isEmpty());
    QVERIFY(onlyA->md5B.isEmpty());
    QCOMPARE(onlyB->kind, tw::HighRowKind::OnlyB);
    QVERIFY(onlyB->md5A.isEmpty());
    QVERIFY(!onlyB->md5B.isEmpty());
}

void Md5CompareHighTests::digest_emptyDirectories()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    QVERIFY(QDir().mkpath(a + QStringLiteral("/nested/empty")));
    QVERIFY(QDir().mkpath(b + QStringLiteral("/also/empty")));

    tw::HighReport report;
    QString error;
    QVERIFY(tw::digestDirectoryPair(a, b, &report, &error));
    QVERIFY(report.rows.isEmpty());
    QCOMPARE(report.issueCount, 0);
    QVERIFY(!report.reachedCap);
}

void Md5CompareHighTests::digest_hiddenChineseAndKnownMd5()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(a + QStringLiteral("/子目录/文件.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(b + QStringLiteral("/子目录/文件.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(a + QStringLiteral("/.secret"), QByteArray("abc")));
    QVERIFY(writeBytes(b + QStringLiteral("/.secret"), QByteArray("abc")));

    QVector<tw::HighProgress> progress;
    tw::HighReport report;
    QString error;
    QVERIFY(tw::digestDirectoryPair(a, b, &report, &error, [&](const tw::HighProgress &step) { progress.append(step); }));
    const tw::HighRow *hidden = findRow(report, QStringLiteral(".secret"));
    const tw::HighRow *chinese = findRow(report, QStringLiteral("子目录/文件.txt"));
    QVERIFY(hidden && chinese);
    QCOMPARE(hidden->kind, tw::HighRowKind::Match);
    QCOMPARE(chinese->kind, tw::HighRowKind::Match);
    QCOMPARE(chinese->md5A, kAbcMd5);
    QVERIFY(!chinese->relativePath.contains(QLatin1Char('\\')));

    QStringList paths;
    for (const tw::HighRow &row : report.rows)
        paths.append(row.relativePath);
    QStringList sorted = paths;
    sorted.sort();
    QCOMPARE(paths, sorted);
    QVERIFY(paths.indexOf(QStringLiteral(".secret")) < paths.indexOf(QStringLiteral("子目录/文件.txt")));

    bool sawHash = false;
    for (const tw::HighProgress &step : progress)
        sawHash = sawHash || !step.scanning;
    QVERIFY(sawHash);
}

void Md5CompareHighTests::digest_symlinkEscape()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString outside = dir.filePath(QStringLiteral("outside"));
    const QString secret = outside + QStringLiteral("/secret.txt");
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(secret, QByteArray("TOP-SECRET-PAYLOAD")));
    QVERIFY(writeBytes(a + QStringLiteral("/ok.txt"), QByteArray("ok")));
    QVERIFY(writeBytes(b + QStringLiteral("/ok.txt"), QByteArray("ok")));
    QVERIFY(QFile::link(secret, a + QStringLiteral("/leak")));
    QVERIFY(QFile::link(outside, a + QStringLiteral("/leakdir")));
    QVERIFY(QFile::link(QStringLiteral(".."), a + QStringLiteral("/up")));

    tw::HighReport report;
    QString error;
    QVERIFY2(tw::digestDirectoryPair(a, b, &report, &error), qPrintable(error));
    const QString blob = issueBlob(report);
    QVERIFY2(blob.contains(QStringLiteral("之外")), qPrintable(blob));
    QVERIFY(findRow(report, QStringLiteral("leak")) != nullptr);
    QCOMPARE(findRow(report, QStringLiteral("leak"))->kind, tw::HighRowKind::Issue);
    QVERIFY(findRow(report, QStringLiteral("leakdir/secret.txt")) == nullptr);
    QVERIFY(findRow(report, QStringLiteral("up/secret.txt")) == nullptr);
    QCOMPARE(report.onlyACount, 0);

    const QByteArray secretMd5 =
        QCryptographicHash::hash(QByteArray("TOP-SECRET-PAYLOAD"), QCryptographicHash::Md5).toHex();
    QVERIFY(!blob.contains(QString::fromLatin1(secretMd5)));
}

void Md5CompareHighTests::digest_symlinkCycleAndInternalLink()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(a + QStringLiteral("/sub/f.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(b + QStringLiteral("/sub/f.txt"), QByteArray("abc")));
    QVERIFY(QFile::link(a + QStringLiteral("/sub"), a + QStringLiteral("/sub/loop")));
    QVERIFY(QFile::link(a + QStringLiteral("/b"), a + QStringLiteral("/a")));
    QVERIFY(QFile::link(a + QStringLiteral("/a"), a + QStringLiteral("/b")));
    QVERIFY(QFile::link(a + QStringLiteral("/sub/f.txt"), a + QStringLiteral("/link.txt")));
    QVERIFY(QFile::link(b + QStringLiteral("/sub/f.txt"), b + QStringLiteral("/link.txt")));
    QVERIFY(QFile::link(a + QStringLiteral("/sub"), a + QStringLiteral("/alias")));
    QVERIFY(QFile::link(b + QStringLiteral("/sub"), b + QStringLiteral("/alias")));

    QElapsedTimer timer;
    timer.start();
    tw::HighReport report;
    QString error;
    QVERIFY2(tw::digestDirectoryPair(a, b, &report, &error), qPrintable(error));
    QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
    const QString blob = issueBlob(report);
    QVERIFY2(blob.contains(QStringLiteral("循环")), qPrintable(blob));
    QVERIFY(findRow(report, QStringLiteral("sub/loop/f.txt")) == nullptr);
    const tw::HighRow *nested = findRow(report, QStringLiteral("sub/f.txt"));
    const tw::HighRow *link = findRow(report, QStringLiteral("link.txt"));
    const tw::HighRow *alias = findRow(report, QStringLiteral("alias/f.txt"));
    QVERIFY(nested && link && alias);
    QCOMPARE(nested->kind, tw::HighRowKind::Match);
    QCOMPARE(link->kind, tw::HighRowKind::Match);
    QCOMPARE(alias->kind, tw::HighRowKind::Match);
    QCOMPARE(link->md5A, kAbcMd5);
    QCOMPARE(alias->md5A, kAbcMd5);
    QCOMPARE(link->md5A, link->md5B);
}

void Md5CompareHighTests::digest_specialFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    QVERIFY(QDir().mkpath(a));
    QVERIFY(writeBytes(a + QStringLiteral("/ok.txt"), QByteArray("ok")));
    QVERIFY(writeBytes(b + QStringLiteral("/ok.txt"), QByteArray("ok")));
    const QString pipe = a + QStringLiteral("/pipe");
    QVERIFY(::mkfifo(QFile::encodeName(pipe).constData(), 0600) == 0);

    QElapsedTimer timer;
    timer.start();
    tw::HighReport report;
    QString error;
    QVERIFY2(tw::digestDirectoryPair(a, b, &report, &error), qPrintable(error));
    QVERIFY(timer.elapsed() < 3000);
    const tw::HighRow *row = findRow(report, QStringLiteral("pipe"));
    QVERIFY(row);
    QCOMPARE(row->kind, tw::HighRowKind::Issue);
    QVERIFY(issueBlob(report).contains(QStringLiteral("不是普通文件")));
    QCOMPARE(report.onlyACount, 0);
}

void Md5CompareHighTests::digest_unreadableFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    const QString blocked = a + QStringLiteral("/secret.txt");
    QVERIFY(writeBytes(blocked, QByteArray("hidden")));
    QVERIFY(writeBytes(b + QStringLiteral("/other.txt"), QByteArray("x")));
    QVERIFY(::chmod(QFile::encodeName(blocked).constData(), 0) == 0);
    ModeGuard guard(blocked, 0600);

    tw::HighReport report;
    QString error;
    QVERIFY2(tw::digestDirectoryPair(a, b, &report, &error), qPrintable(error));
    const tw::HighRow *row = findRow(report, QStringLiteral("secret.txt"));
    QVERIFY(row);
    QCOMPARE(row->kind, tw::HighRowKind::Issue);
    QVERIFY(issueBlob(report).contains(QStringLiteral("无法读取")));
    QVERIFY(row->md5A.isEmpty());
}

void Md5CompareHighTests::digest_unreadableDirectory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    const QString nested = a + QStringLiteral("/hidden");
    QVERIFY(writeBytes(nested + QStringLiteral("/child.txt"), QByteArray("child")));
    QVERIFY(QDir().mkpath(b));
    QVERIFY(::chmod(QFile::encodeName(nested).constData(), 0) == 0);
    ModeGuard guard(nested, 0700);

    tw::HighReport report;
    QString error;
    QVERIFY2(tw::digestDirectoryPair(a, b, &report, &error), qPrintable(error));
    QVERIFY(findRow(report, QStringLiteral("hidden/child.txt")) == nullptr);
    QVERIFY(issueBlob(report).contains(QStringLiteral("无法读取")));
    QVERIFY(issueBlob(report).contains(QStringLiteral("hidden")));
}

void Md5CompareHighTests::digest_cancelDropsPartial()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(a + QStringLiteral("/one.txt"), QByteArray("one")));
    QVERIFY(writeBytes(b + QStringLiteral("/two.txt"), QByteArray("two")));

    tw::HighReport immediate;
    QString error;
    std::atomic<bool> stopNow{true};
    QVERIFY(tw::digestDirectoryPair(a, b, &immediate, &error, {}, [&] { return stopNow.load(); }));
    QVERIFY(immediate.cancelled);
    QVERIFY(immediate.rows.isEmpty());
    QCOMPARE(immediate.matchCount, 0);
    QCOMPARE(immediate.onlyACount, 0);
    QVERIFY(error.isEmpty());

    std::atomic<int> seen{0};
    std::atomic<bool> stopLater{false};
    tw::HighReport later;
    QVERIFY(tw::digestDirectoryPair(
        a, b, &later, &error,
        [&](const tw::HighProgress &) {
            if (seen.fetch_add(1) >= 0)
                stopLater.store(true);
        },
        [&] { return stopLater.load(); }));
    QVERIFY(later.cancelled);
    QVERIFY(later.rows.isEmpty());
    QCOMPARE(later.issueCount, 0);
    QVERIFY(seen.load() >= 1);
}

void Md5CompareHighTests::digest_hashCancelAndOutsideFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("root"));
    QVERIFY(QDir().mkpath(root));
    const QString big = root + QStringLiteral("/big.bin");
    QFile file(big);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(3 * 256 * 1024));
    file.close();

    const QString canonical = QFileInfo(root).canonicalFilePath();
    QString hex = QStringLiteral("stale");
    QString error;
    int calls = 0;
    QElapsedTimer timer;
    timer.start();
    const tw::FileMd5State state = tw::hashContainedFileMd5(big, canonical, &hex, &error, [&] {
        return ++calls >= 3;
    });
    QVERIFY(timer.elapsed() < 3000);
    QCOMPARE(state, tw::FileMd5State::Stopped);
    QVERIFY(hex.isEmpty());
    QVERIFY(calls >= 3);

    const QString outside = dir.filePath(QStringLiteral("sibling.bin"));
    QVERIFY(writeBytes(outside, QByteArray("nope")));
    hex = QStringLiteral("stale");
    const tw::FileMd5State rejected = tw::hashContainedFileMd5(outside, canonical, &hex, &error);
    QCOMPARE(rejected, tw::FileMd5State::Failed);
    QVERIFY(hex.isEmpty());
    QVERIFY(!error.isEmpty());
}

void Md5CompareHighTests::digest_depthAndFileCaps()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(a + QStringLiteral("/root.txt"), QByteArray("r")));
    QVERIFY(writeBytes(b + QStringLiteral("/root.txt"), QByteArray("r")));
    QVERIFY(writeBytes(a + QStringLiteral("/sub/file.txt"), QByteArray("s")));
    QVERIFY(writeBytes(b + QStringLiteral("/sub/file.txt"), QByteArray("s")));
    QVERIFY(writeBytes(a + QStringLiteral("/sub/deep/x.txt"), QByteArray("x")));
    QVERIFY(writeBytes(b + QStringLiteral("/sub/deep/x.txt"), QByteArray("x")));

    tw::HighCaps depth;
    depth.depth = 2;
    tw::HighReport deepReport;
    QString error;
    QVERIFY(tw::digestDirectoryPair(a, b, &deepReport, &error, {}, {}, depth));
    QVERIFY(findRow(deepReport, QStringLiteral("root.txt")));
    QVERIFY(findRow(deepReport, QStringLiteral("sub/file.txt")));
    QVERIFY(findRow(deepReport, QStringLiteral("sub/deep/x.txt")) == nullptr);
    QVERIFY(issueBlob(deepReport).contains(QStringLiteral("sub/deep")));
    QVERIFY(issueBlob(deepReport).contains(QStringLiteral("层级")));
    QVERIFY(!deepReport.reachedCap);

    QVERIFY(writeBytes(a + QStringLiteral("/a.txt"), QByteArray("a")));
    QVERIFY(writeBytes(a + QStringLiteral("/b.txt"), QByteArray("b")));
    QVERIFY(writeBytes(a + QStringLiteral("/c.txt"), QByteArray("c")));
    QVERIFY(writeBytes(b + QStringLiteral("/a.txt"), QByteArray("a")));
    QVERIFY(writeBytes(b + QStringLiteral("/b.txt"), QByteArray("b")));
    QVERIFY(writeBytes(b + QStringLiteral("/c.txt"), QByteArray("c")));
    tw::HighCaps files;
    files.files = 1;
    tw::HighReport capped;
    QVERIFY(tw::digestDirectoryPair(a, b, &capped, &error, {}, {}, files));
    QVERIFY(findRow(capped, QStringLiteral("a.txt")));
    QVERIFY(findRow(capped, QStringLiteral("b.txt")) == nullptr);
    QVERIFY(findRow(capped, QStringLiteral("c.txt")) == nullptr);
    QVERIFY(capped.reachedCap);
    QVERIFY(issueBlob(capped).contains(QStringLiteral("上限")));
    QVERIFY(findRow(capped, QStringLiteral("（扫描上限）")));
}

void Md5CompareHighTests::digest_doesNotModifyTrees()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(a + QStringLiteral("/f.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(b + QStringLiteral("/f.txt"), QByteArray("xyz")));
    QVERIFY(QFile::link(a + QStringLiteral("/f.txt"), a + QStringLiteral("/link")));
    const QStringList beforeA = snapshotTree(a);
    const QStringList beforeB = snapshotTree(b);
    tw::HighReport report;
    QString error;
    QVERIFY(tw::digestDirectoryPair(a, b, &report, &error));
    QCOMPARE(snapshotTree(a), beforeA);
    QCOMPARE(snapshotTree(b), beforeB);
}

void Md5CompareHighTests::digest_rejectsMissingAndFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString real = dir.filePath(QStringLiteral("real"));
    QVERIFY(QDir().mkpath(real));
    const QString missing = dir.filePath(QStringLiteral("missing"));
    const QString file = dir.filePath(QStringLiteral("file.txt"));
    QVERIFY(writeBytes(file, QByteArray("x")));

    tw::HighReport report;
    QString error;
    QVERIFY(!tw::digestDirectoryPair(missing, real, &report, &error));
    QVERIFY(error.contains(QStringLiteral("不存在")));
    QVERIFY(report.rows.isEmpty());

    QVERIFY(!tw::digestDirectoryPair(file, real, &report, &error));
    QVERIFY(error.contains(QStringLiteral("不是目录")));
    QVERIFY(report.rows.isEmpty());
}

void Md5CompareHighTests::csv_bomQuotingAndBlockedPaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("tree"));
    const QString b = dir.filePath(QStringLiteral("tree-b"));
    QVERIFY(writeBytes(a + QStringLiteral("/a\"b,c.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(b + QStringLiteral("/a\"b,c.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(a + QStringLiteral("/文件.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(b + QStringLiteral("/文件.txt"), QByteArray("abc")));

    tw::HighReport report;
    QString error;
    QVERIFY2(tw::digestDirectoryPair(a, b, &report, &error), qPrintable(error));
    const QString outside = dir.filePath(QStringLiteral("out.csv"));
    QVERIFY2(tw::writeHighReportCsv(outside, report, &error), qPrintable(error));

    QFile written(outside);
    QVERIFY(written.open(QIODevice::ReadOnly));
    const QByteArray raw = written.readAll();
    QVERIFY(raw.startsWith(QByteArray("\xEF\xBB\xBF", 3)));
    const QString text = QString::fromUtf8(raw.mid(3));
    const QString header = text.section(QLatin1Char('\n'), 0, 0);
    QCOMPARE(header, QStringLiteral("\"相对路径\",\"状态\",\"目录A的MD5\",\"目录B的MD5\",\"说明\""));
    QVERIFY(text.contains(QStringLiteral("\"a\"\"b,c.txt\"")));
    QVERIFY(text.contains(QStringLiteral("\"文件.txt\"")));
    QVERIFY(text.contains(QStringLiteral("\"相同\"")));
    QVERIFY(text.contains(kAbcMd5));
    QVERIFY(text.endsWith(QLatin1Char('\n')));

    QVERIFY(!tw::writeHighReportCsv(dir.filePath(QStringLiteral("missing-dir/out.csv")), report, &error));
    QVERIFY(!error.isEmpty());

    const QString blocked = tw::highCsvInsideTreeReason(a, b, a + QStringLiteral("/out.csv"));
    QVERIFY(blocked.contains(QStringLiteral("不能把导出文件保存到被比较的目录中")));
    QVERIFY(tw::highCsvInsideTreeReason(a, b, outside).isEmpty());

    const QString sibling = a + QStringLiteral("-extra");
    QVERIFY(QDir().mkpath(sibling));
    QVERIFY(tw::highCsvInsideTreeReason(a, b, sibling + QStringLiteral("/out.csv")).isEmpty());

    const QString alias = dir.filePath(QStringLiteral("alias"));
    QVERIFY(QFile::link(a, alias));
    QVERIFY(tw::highCsvInsideTreeReason(a, b, alias + QStringLiteral("/out.csv")).contains(QStringLiteral("不能把")));
}

void Md5CompareHighTests::filter_statusAndPath()
{
    tw::HighRow match;
    match.relativePath = QStringLiteral("Diff.TXT");
    match.kind = tw::HighRowKind::Match;
    tw::HighRow onlyA;
    onlyA.relativePath = QStringLiteral("onlyA.txt");
    onlyA.kind = tw::HighRowKind::OnlyA;

    QVERIFY(tw::highRowVisible(match, -1, QStringLiteral("  diff ")));
    QVERIFY(!tw::highRowVisible(match, static_cast<int>(tw::HighRowKind::Mismatch), QString()));
    QVERIFY(tw::highRowVisible(match, static_cast<int>(tw::HighRowKind::Match), QStringLiteral("DIFF")));
    QVERIFY(!tw::highRowVisible(match, static_cast<int>(tw::HighRowKind::Match), QStringLiteral("nope")));
    QVERIFY(tw::highRowVisible(onlyA, static_cast<int>(tw::HighRowKind::OnlyA), QString()));
    QVERIFY(!tw::highRowVisible(onlyA, static_cast<int>(tw::HighRowKind::OnlyA), QStringLiteral("diff")));
    QCOMPARE(tw::highRowKindText(tw::HighRowKind::Mismatch), QStringLiteral("内容不同"));
}

void Md5CompareHighTests::ui_compareLockFilterAndExport()
{
    Harness harness;
    QString error;
    QVERIFY2(harness.open(&error), qPrintable(error));
    const QString a = harness.dir.filePath(QStringLiteral("a"));
    const QString b = harness.dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(a + QStringLiteral("/same.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(b + QStringLiteral("/same.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(a + QStringLiteral("/diff.txt"), QByteArray("one")));
    QVERIFY(writeBytes(b + QStringLiteral("/diff.txt"), QByteArray("two")));
    QVERIFY(writeBytes(a + QStringLiteral("/onlyA.txt"), QByteArray("a")));
    QVERIFY(writeBytes(b + QStringLiteral("/onlyB.txt"), QByteArray("b")));

    harness.page->show();
    QVERIFY(QTest::qWaitForWindowExposed(harness.page));
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirA"))->setText(a);
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirB"))->setText(b);
    auto *start = harness.page->findChild<QPushButton *>(QStringLiteral("md5HighStart"));
    start->click();
    QVERIFY(harness.page->isBusy());
    QVERIFY(harness.platform->gate()->isLocked());
    QVERIFY(harness.platform->gate()->reason().contains(QStringLiteral("正在比较目录 MD5")));
    QCOMPARE(harness.platform->status()->current(harness.tool.info().id).state, tw::StatusState::Busy);

    QTRY_VERIFY_WITH_TIMEOUT(!harness.page->isBusy(), 10000);
    QVERIFY(!harness.platform->gate()->isLocked());
    QCOMPARE(harness.platform->status()->current(harness.tool.info().id).state, tw::StatusState::Success);

    auto *counts = harness.page->findChild<QLabel *>(QStringLiteral("md5HighCounts"));
    const QString summary = labelText(counts);
    QVERIFY(summary.contains(QStringLiteral("相同 1")));
    QVERIFY(summary.contains(QStringLiteral("内容不同 1")));
    QVERIFY(summary.contains(QStringLiteral("仅在目录 A 1")));
    QVERIFY(summary.contains(QStringLiteral("仅在目录 B 1")));
    QVERIFY(summary.contains(QStringLiteral("问题 0")));

    auto *table = harness.page->findChild<QTableView *>(QStringLiteral("md5HighTable"));
    auto *filter = harness.page->findChild<QComboBox *>(QStringLiteral("md5HighFilter"));
    const int onlyAIndex = filter->findText(QStringLiteral("仅在目录 A"));
    QVERIFY(onlyAIndex >= 0);
    filter->setCurrentIndex(onlyAIndex);
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->index(0, 0).data(Qt::UserRole).toString(), QStringLiteral("onlyA.txt"));

    filter->setCurrentIndex(0);
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighSearch"))->setText(QStringLiteral("  DiFf "));
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->index(0, 0).data(Qt::UserRole).toString(), QStringLiteral("diff.txt"));

    const QString outside = harness.dir.filePath(QStringLiteral("out.csv"));
    QVERIFY2(harness.page->exportReportTo(outside, &error), qPrintable(error));
    QFile exported(outside);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QVERIFY(exported.readAll().startsWith(QByteArray("\xEF\xBB\xBF", 3)));

    const QString inside = a + QStringLiteral("/out.csv");
    QVERIFY(!harness.page->exportReportTo(inside, &error));
    QVERIFY(error.contains(QStringLiteral("不能把导出文件保存到被比较的目录中")));
    QVERIFY(!QFileInfo::exists(inside));
    QVERIFY(harness.page->findChild<QPushButton *>(QStringLiteral("md5HighExport"))->isEnabled());
}

void Md5CompareHighTests::ui_treesSyncDetailAndSort()
{
    Harness harness;
    QString error;
    QVERIFY2(harness.open(&error), qPrintable(error));
    const QString a = harness.dir.filePath(QStringLiteral("a"));
    const QString b = harness.dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(a + QStringLiteral("/same.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(b + QStringLiteral("/same.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(a + QStringLiteral("/diff.txt"), QByteArray("one")));
    QVERIFY(writeBytes(b + QStringLiteral("/diff.txt"), QByteArray("two")));
    QVERIFY(writeBytes(a + QStringLiteral("/onlyA.txt"), QByteArray("a")));
    QVERIFY(writeBytes(b + QStringLiteral("/onlyB.txt"), QByteArray("b")));
    QVERIFY(writeBytes(a + QStringLiteral("/子目录/文件.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(b + QStringLiteral("/子目录/文件.txt"), QByteArray("xyz")));

    harness.page->resize(1180, 760);
    harness.page->show();
    QVERIFY(QTest::qWaitForWindowExposed(harness.page));
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirA"))->setText(a);
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirB"))->setText(b);
    harness.page->findChild<QPushButton *>(QStringLiteral("md5HighStart"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.page->isBusy(), 10000);

    auto *treeA = harness.page->findChild<QTreeView *>(QStringLiteral("md5HighTreeA"));
    auto *treeB = harness.page->findChild<QTreeView *>(QStringLiteral("md5HighTreeB"));
    auto *table = harness.page->findChild<QTableView *>(QStringLiteral("md5HighTable"));
    QVERIFY(treeA && treeB && table);
    QVERIFY(treeA->height() >= 120);
    QVERIFY(table->height() >= 120);

    const QModelIndex onlyA = findTreePath(treeA, QStringLiteral("onlyA.txt"));
    QVERIFY(onlyA.isValid());
    treeA->setCurrentIndex(onlyA);
    QCOMPARE(treeB->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("onlyA.txt"));
    QCOMPARE(table->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("onlyA.txt"));

    const QModelIndex diffOnB = findTreePath(treeB, QStringLiteral("子目录/文件.txt"));
    QVERIFY(diffOnB.isValid());
    treeB->setCurrentIndex(diffOnB);
    QCOMPARE(treeA->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("子目录/文件.txt"));
    QCOMPARE(table->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("子目录/文件.txt"));

    treeA->setFocus();
    QTest::keyClick(treeA, Qt::Key_Return);
    QApplication::processEvents();
    auto *dialog = harness.page->findChild<QDialog *>(QStringLiteral("md5HighDetail"));
    QVERIFY(dialog);
    QVERIFY(dialog->isVisible());
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("md5HighDetailPath"))->text(),
             QStringLiteral("子目录/文件.txt"));
    QVERIFY(!dialog->findChild<QLabel *>(QStringLiteral("md5HighDetailMd5A"))->text().isEmpty());
    QVERIFY(dialog->findChild<QLabel *>(QStringLiteral("md5HighDetailMd5A"))->text()
            != dialog->findChild<QLabel *>(QStringLiteral("md5HighDetailMd5B"))->text());
    dialog->hide();

    auto *filter = harness.page->findChild<QComboBox *>(QStringLiteral("md5HighFilter"));
    filter->setCurrentIndex(filter->findText(QStringLiteral("仅在目录 A")));
    QVERIFY(findTreePath(treeA, QStringLiteral("onlyA.txt")).isValid());
    QVERIFY(!findTreePath(treeA, QStringLiteral("diff.txt")).isValid());
    QVERIFY(!findTreePath(treeB, QStringLiteral("same.txt")).isValid());
    filter->setCurrentIndex(0);
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighSearch"))->clear();

    table->sortByColumn(0, Qt::AscendingOrder);
    QCOMPARE(table->model()->index(0, 0).data(Qt::UserRole).toString(), QStringLiteral("diff.txt"));
    table->sortByColumn(0, Qt::DescendingOrder);
    QCOMPARE(table->model()->index(0, 0).data(Qt::UserRole).toString(), QStringLiteral("子目录/文件.txt"));
}

void Md5CompareHighTests::ui_layoutAtTypicalSizes()
{
    Harness harness;
    QString error;
    QVERIFY2(harness.open(&error), qPrintable(error));
    harness.page->show();
    QVERIFY(QTest::qWaitForWindowExposed(harness.page));

    const QList<QSize> sizes{{520, 420}, {640, 480}, {1100, 720}, {1180, 760}};
    for (const QSize &size : sizes) {
        harness.page->resize(size);
        if (harness.page->layout())
            harness.page->layout()->activate();
        QApplication::processEvents();
        QCOMPARE(harness.page->width(), size.width());
        QCOMPARE(harness.page->height(), size.height());
        const QString problem = layoutProblem(harness.page);
        QVERIFY2(problem.isEmpty(), qPrintable(QStringLiteral("%1x%2 %3")
                                                    .arg(size.width())
                                                    .arg(size.height())
                                                    .arg(problem)));
    }
}

void Md5CompareHighTests::ui_saveReferenceShots()
{
    const QByteArray shotDir = qgetenv("MD5_HIGH_SHOT_DIR");
    if (shotDir.isEmpty())
        return;
    QVERIFY(QDir().mkpath(QString::fromLocal8Bit(shotDir)));

    Harness harness;
    QString error;
    QVERIFY2(harness.open(&error), qPrintable(error));
    harness.page->setStyleSheet(tw::Theme::workbench().styleSheet());
    harness.page->resize(1180, 760);
    harness.page->show();
    QVERIFY(QTest::qWaitForWindowExposed(harness.page));
    QApplication::processEvents();
    QVERIFY(harness.page->grab().save(QString::fromLocal8Bit(shotDir) + QStringLiteral("/empty-1180x760.png")));

    const QString a = harness.dir.filePath(QStringLiteral("甲"));
    const QString b = harness.dir.filePath(QStringLiteral("乙"));
    QVERIFY(writeBytes(a + QStringLiteral("/same.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(b + QStringLiteral("/same.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(a + QStringLiteral("/diff.txt"), QByteArray("one")));
    QVERIFY(writeBytes(b + QStringLiteral("/diff.txt"), QByteArray("two")));
    QVERIFY(writeBytes(a + QStringLiteral("/onlyA.txt"), QByteArray("left")));
    QVERIFY(writeBytes(b + QStringLiteral("/onlyB.txt"), QByteArray("right")));
    QVERIFY(writeBytes(a + QStringLiteral("/子目录/文件.txt"), QByteArray("abc")));
    QVERIFY(writeBytes(b + QStringLiteral("/子目录/文件.txt"), QByteArray("xyz")));
    QVERIFY(QFile::link(harness.dir.filePath(QStringLiteral("outside")), a + QStringLiteral("/越界")));
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirA"))->setText(a);
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirB"))->setText(b);
    harness.page->findChild<QPushButton *>(QStringLiteral("md5HighStart"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.page->isBusy(), 10000);
    harness.page->findChild<QTreeView *>(QStringLiteral("md5HighTreeA"))->setCurrentIndex(
        findTreePath(harness.page->findChild<QTreeView *>(QStringLiteral("md5HighTreeA")),
                     QStringLiteral("子目录/文件.txt")));
    QApplication::processEvents();
    QVERIFY(harness.page->grab().save(QString::fromLocal8Bit(shotDir) + QStringLiteral("/result-1180x760.png")));

    QTemporaryDir windowDir;
    QVERIFY(windowDir.isValid());
    tw::ToolRegistry registry;
    QString addError;
    QVERIFY(registry.add(std::make_unique<tw::Md5CompareHighTool>(), &addError));
    auto platform = std::make_unique<tw::PlatformServices>(tw::DataPaths(windowDir.filePath(QStringLiteral("data"))));
    tw::MainWindow window(std::move(platform), std::move(registry));
    window.setCloseGuard([](const QString &) { return true; });
    window.resize(1180, 760);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.selectToolById(QStringLiteral("md5-compare-high")));
    QApplication::processEvents();
    QVERIFY(window.grab().save(QString::fromLocal8Bit(shotDir) + QStringLiteral("/window-empty-1180x760.png")));

    auto *embedded = window.findChild<tw::Md5CompareHighPage *>();
    QVERIFY(embedded);
    embedded->findChild<QLineEdit *>(QStringLiteral("md5HighDirA"))->setText(a);
    embedded->findChild<QLineEdit *>(QStringLiteral("md5HighDirB"))->setText(b);
    embedded->findChild<QPushButton *>(QStringLiteral("md5HighStart"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(!embedded->isBusy(), 10000);
    auto *embeddedTree = embedded->findChild<QTreeView *>(QStringLiteral("md5HighTreeA"));
    embeddedTree->setCurrentIndex(findTreePath(embeddedTree, QStringLiteral("子目录/文件.txt")));
    QApplication::processEvents();
    QVERIFY(window.grab().save(QString::fromLocal8Bit(shotDir) + QStringLiteral("/window-result-1180x760.png")));
}

void Md5CompareHighTests::ui_mainWindowShowsTable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    tw::ToolRegistry registry;
    auto owned = std::make_unique<tw::Md5CompareHighTool>();
    tw::Md5CompareHighTool *tool = owned.get();
    QString error;
    QVERIFY(registry.add(std::move(owned), &error));
    auto platform = std::make_unique<tw::PlatformServices>(tw::DataPaths(dir.filePath(QStringLiteral("data"))));
    tw::MainWindow window(std::move(platform), std::move(registry));
    window.setCloseGuard([](const QString &) { return true; });
    window.resize(1180, 760);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.selectToolById(QStringLiteral("md5-compare-high")));
    QApplication::processEvents();

    auto *table = window.findChild<QTableView *>(QStringLiteral("md5HighTable"));
    QVERIFY(table);
    QVERIFY(table->isVisible());
    QVERIFY(table->height() >= 40);
    const QRect tableRect(table->mapTo(&window, QPoint(0, 0)), table->size());
    QVERIFY(window.rect().contains(tableRect));

    tw::Md5CompareHighPage *page = tool->page();
    QVERIFY(page);
    const QString a = dir.filePath(QStringLiteral("a"));
    const QString b = dir.filePath(QStringLiteral("b"));
    QVERIFY(QDir().mkpath(a));
    QVERIFY(QDir().mkpath(b));
    page->findChild<QLineEdit *>(QStringLiteral("md5HighDirA"))->setText(a);
    page->findChild<QLineEdit *>(QStringLiteral("md5HighDirB"))->setText(b);
    page->findChild<QPushButton *>(QStringLiteral("md5HighStart"))->click();
    QVERIFY(page->isBusy());
    QVERIFY(window.navigationLocked());
    QVERIFY(window.statusStrip()->lockText().contains(QStringLiteral("正在比较目录 MD5")));
    QVERIFY(window.statusStrip()->status().state == tw::StatusState::Busy);
    page->findChild<QPushButton *>(QStringLiteral("md5HighCancel"))->click();
    QVERIFY(!page->isBusy());
    QVERIFY(!window.navigationLocked());
    QCOMPARE(table->model()->rowCount(), 0);
}

void Md5CompareHighTests::ui_cancelReleasesLockAndDropsRows()
{
    Harness harness;
    QString error;
    QVERIFY2(harness.open(&error), qPrintable(error));
    const QString a = harness.dir.filePath(QStringLiteral("a"));
    const QString b = harness.dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(a + QStringLiteral("/big.bin"), QByteArray(256 * 1024, 'x')));
    QVERIFY(writeBytes(b + QStringLiteral("/big.bin"), QByteArray(256 * 1024, 'y')));
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirA"))->setText(a);
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirB"))->setText(b);
    harness.page->findChild<QPushButton *>(QStringLiteral("md5HighStart"))->click();
    QVERIFY(harness.page->isBusy());
    QVERIFY(harness.platform->gate()->isLocked());

    harness.page->findChild<QPushButton *>(QStringLiteral("md5HighCancel"))->click();
    QVERIFY(!harness.page->isBusy());
    QVERIFY(!harness.platform->gate()->isLocked());
    QCOMPARE(harness.platform->status()->current(harness.tool.info().id).state, tw::StatusState::Info);
    auto *table = harness.page->findChild<QTableView *>(QStringLiteral("md5HighTable"));
    QCOMPARE(table->model()->rowCount(), 0);
    QVERIFY(!harness.page->findChild<QPushButton *>(QStringLiteral("md5HighExport"))->isEnabled());

    QTest::qWait(50);
    QCOMPARE(table->model()->rowCount(), 0);
    QVERIFY(!harness.platform->gate()->isLocked());
    QVERIFY(!harness.page->isBusy());
}

void Md5CompareHighTests::ui_failureReleasesLock()
{
    Harness harness;
    QString error;
    QVERIFY2(harness.open(&error), qPrintable(error));
    const QString file = harness.dir.filePath(QStringLiteral("not-a-dir.txt"));
    const QString real = harness.dir.filePath(QStringLiteral("real"));
    QVERIFY(writeBytes(file, QByteArray("x")));
    QVERIFY(QDir().mkpath(real));
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirA"))->setText(file);
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirB"))->setText(real);
    harness.page->findChild<QPushButton *>(QStringLiteral("md5HighStart"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.page->isBusy(), 10000);
    QVERIFY(!harness.platform->gate()->isLocked());
    QCOMPARE(harness.platform->status()->current(harness.tool.info().id).state, tw::StatusState::Error);
    QCOMPARE(harness.page->findChild<QTableView *>(QStringLiteral("md5HighTable"))->model()->rowCount(), 0);
    QVERIFY(!harness.page->findChild<QPushButton *>(QStringLiteral("md5HighExport"))->isEnabled());
}

void Md5CompareHighTests::ui_shutdownReleasesLock()
{
    Harness harness;
    QString error;
    QVERIFY2(harness.open(&error), qPrintable(error));
    const QString a = harness.dir.filePath(QStringLiteral("a"));
    const QString b = harness.dir.filePath(QStringLiteral("b"));
    QVERIFY(writeBytes(a + QStringLiteral("/big.bin"), QByteArray(256 * 1024, 'x')));
    QVERIFY(writeBytes(b + QStringLiteral("/big.bin"), QByteArray(256 * 1024, 'y')));
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirA"))->setText(a);
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirB"))->setText(b);
    harness.page->findChild<QPushButton *>(QStringLiteral("md5HighStart"))->click();
    QVERIFY(harness.platform->gate()->isLocked());
    harness.tool.shutdown();
    QVERIFY(!harness.platform->gate()->isLocked());
    QVERIFY(!harness.page->isBusy());
    QTest::qWait(50);
    QVERIFY(!harness.platform->gate()->isLocked());
    QCOMPARE(harness.page->findChild<QTableView *>(QStringLiteral("md5HighTable"))->model()->rowCount(), 0);
}

void Md5CompareHighTests::ui_memoryStaysInDataDir()
{
    Harness harness;
    QString error;
    QVERIFY2(harness.open(&error), qPrintable(error));
    const QString a = harness.dir.filePath(QStringLiteral("甲目录"));
    const QString b = harness.dir.filePath(QStringLiteral("乙目录"));
    QVERIFY(QDir().mkpath(a));
    QVERIFY(QDir().mkpath(b));
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirA"))->setText(a);
    harness.page->findChild<QLineEdit *>(QStringLiteral("md5HighDirB"))->setText(b);
    harness.page->findChild<QPushButton *>(QStringLiteral("md5HighStart"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.page->isBusy(), 10000);

    QString dirError;
    const QString dataDir = harness.platform->servicesFor(harness.tool.info().id).dataDir(&dirError);
    QVERIFY2(!dataDir.isEmpty(), qPrintable(dirError));
    QVERIFY(dataDir.contains(QStringLiteral("tools/md5-compare-high")));
    const QString memory = QDir(dataDir).filePath(QStringLiteral("watched-folders.json"));
    QFile file(memory);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray payload = file.readAll();
    file.close();
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    QCOMPARE(obj.value(QStringLiteral("directoryA")).toString(), a);
    QCOMPARE(obj.value(QStringLiteral("directoryB")).toString(), b);
    QVERIFY(!snapshotTree(a).contains(memory));
    QVERIFY(!snapshotTree(b).contains(memory));

    delete harness.page;
    harness.page = nullptr;
    QFile again(memory);
    QVERIFY(again.open(QIODevice::ReadOnly));
    QCOMPARE(again.readAll(), payload);
}

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    QApplication app(argc, argv);
    Md5CompareHighTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "md5_compare_high_test.moc"
