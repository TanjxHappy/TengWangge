#include "core/data_paths.h"
#include "core/platform_services.h"
#include "shell/elided_label.h"
#include "shell/main_window.h"
#include "shell/tool_rail.h"
#include "shell/tool_registry.h"
#include "shell/tool_view.h"
#include "tools/filter_unique_high/bounded_tree.h"
#include "tools/filter_unique_high/high_filter_tool.h"
#include "tools/filter_unique_high/term_list.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QSplitter>
#include <QTableView>
#include <QTemporaryDir>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtTest>

#include <atomic>

namespace {

QString writeFile(const QString &path, const QByteArray &bytes = QByteArrayLiteral("x"))
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.dir().path()))
        return QStringLiteral("无法创建目录");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return file.errorString();
    if (file.write(bytes) != bytes.size())
        return QStringLiteral("写入不完整");
    return {};
}

const tw::ListedFile *findFile(const tw::TreeWalk &walk, const QString &relative)
{
    for (const tw::ListedFile &file : walk.files) {
        if (file.relativePath == relative)
            return &file;
    }
    return nullptr;
}

QString issueText(const tw::TreeWalk &walk)
{
    QStringList lines;
    for (const tw::TreeIssue &issue : walk.issues)
        lines.append(issue.path + QLatin1Char(' ') + issue.reason);
    return lines.join(QLatin1Char('\n'));
}

int rowWithPath(QTableView *table, const QString &path)
{
    QAbstractItemModel *model = table->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, 0).data().toString() == path)
            return row;
    }
    return -1;
}

QString cellText(QTableView *table, int row, int column)
{
    return table->model()->index(row, column).data().toString();
}

struct Harness
{
    QTemporaryDir temp;
    std::unique_ptr<tw::PlatformServices> platform;
    tw::HighFilterTool tool;
    QWidget host;
    tw::HighFilterPage *page = nullptr;
};

QString openHarness(Harness *harness)
{
    if (!harness->temp.isValid())
        return QStringLiteral("临时目录不可用");
    harness->platform = std::make_unique<tw::PlatformServices>(
        tw::DataPaths(harness->temp.filePath(QStringLiteral("data"))));
    tw::ToolContext context;
    context.info = harness->tool.info();
    context.services = harness->platform->servicesFor(context.info.id);
    context.dialogParent = &harness->host;
    QString error;
    if (!harness->tool.initialize(context, &error))
        return error;
    const QVector<tw::ToolPage> pages = harness->tool.createPages();
    if (pages.size() != 1 || pages.at(0).widget == nullptr)
        return QStringLiteral("页面创建失败");
    harness->page = harness->tool.page();
    if (harness->page == nullptr)
        return QStringLiteral("页面类型不符");
    auto *layout = new QVBoxLayout(&harness->host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(pages.at(0).widget);
    harness->host.resize(1000, 680);
    harness->host.show();
    if (!QTest::qWaitForWindowExposed(&harness->host))
        return QStringLiteral("窗口未能显示");
    return {};
}

void waitIdle(tw::HighFilterPage *page)
{
    QTRY_VERIFY_WITH_TIMEOUT(!page->isBusy(), 8000);
}

struct UnlinkLater
{
    QString path;
    ~UnlinkLater() { QFile::remove(path); }
};

struct RestoreMode
{
    QString path;
    QFile::Permissions mode;
    ~RestoreMode() { QFile::setPermissions(path, mode); }
};

} // namespace

class HighFilterTests : public QObject
{
    Q_OBJECT

private slots:
    void identifierRejectsBadDataRoot();
    void distillDropsBlanksAndKeepsFirstSpelling();
    void substringMatchUsesCaseFoldOnPathAndName();
    void walkSkipsEscapingLinksCyclesAndBrokenLinks();
    void unreadableDirectoryIsAnIssue();
    void removalRefusesDirectoriesSymlinksAndOutside();
    void pageSummarizesFuzzyMatchAndSearch();
    void deletionRequiresSecondConfirmation();
    void removalFailureNamesTheReason();
    void cancelWalkAndRemovalTouchNothing();
    void importUtf8DedupesAndRejectsInvalid();
    void stateStaysInsideDataDir();
    void resultTableStaysVisibleInTypicalWindow();
};

void HighFilterTests::identifierRejectsBadDataRoot()
{
    tw::HighFilterTool tool;
    const tw::ToolInfo info = tool.info();
    QCOMPARE(info.id, QStringLiteral("filter-unique-high"));
    QVERIFY(!info.id.contains(QLatin1Char('_')));
    QCOMPARE(info.title, QStringLiteral("筛选去重high"));
    QVERIFY(!info.summary.trimmed().isEmpty());
    QCOMPARE(info.order, 40);

    tw::PlatformServices blocked(tw::DataPaths(QStringLiteral("/")));
    tw::ToolContext context;
    context.info = info;
    context.services = blocked.servicesFor(info.id);
    QString error;
    QVERIFY(!tool.initialize(context, &error));
    QVERIFY(error.contains(QStringLiteral("根目录")));
}

void HighFilterTests::distillDropsBlanksAndKeepsFirstSpelling()
{
    const QVector<tw::Term> terms =
        tw::distillTerms(QStringLiteral("  Alpha  \n\nAlpha\r\nb\r\n  B  \n   \n说明\n"));
    QCOMPARE(terms.size(), 3);
    QCOMPARE(terms.at(0).shown, QStringLiteral("Alpha"));
    QCOMPARE(terms.at(1).shown, QStringLiteral("b"));
    QCOMPARE(terms.at(2).shown, QStringLiteral("说明"));
    QVERIFY(tw::distillTerms(QStringLiteral(" \n\t")).isEmpty());
}

void HighFilterTests::substringMatchUsesCaseFoldOnPathAndName()
{
    const QVector<tw::Term> terms = tw::distillTerms(QStringLiteral("report_2024\n说明"));
    QCOMPARE(tw::termsHitting(terms, QStringLiteral("notes/my_report_2024.md"),
                              QStringLiteral("my_report_2024.md")),
             QStringList({QStringLiteral("report_2024")}));
    QCOMPARE(tw::termsHitting(terms, QStringLiteral("资料/说明.txt"), QStringLiteral("说明.txt")),
             QStringList({QStringLiteral("说明")}));
    QVERIFY(tw::termsHitting(terms, QStringLiteral("keep.txt"), QStringLiteral("keep.txt")).isEmpty());
    QVERIFY(tw::termsHitting(tw::distillTerms(QStringLiteral("SpecialRoot")), QStringLiteral("a.txt"),
                             QStringLiteral("a.txt"))
                .isEmpty());
    QVERIFY(tw::termsHitting(tw::distillTerms(QStringLiteral("report.txt")),
                             QStringLiteral("report_2024.TXT"), QStringLiteral("report_2024.TXT"))
                .isEmpty());
    QCOMPARE(tw::termsHitting(tw::distillTerms(QStringLiteral("REPORT")),
                              QStringLiteral("Report_2024.TXT"), QStringLiteral("Report_2024.TXT")),
             QStringList({QStringLiteral("REPORT")}));
    QCOMPARE(tw::termsHitting(tw::distillTerms(QStringLiteral("notes")), QStringLiteral("notes/a.txt"),
                              QStringLiteral("a.txt")),
             QStringList({QStringLiteral("notes")}));
}

void HighFilterTests::walkSkipsEscapingLinksCyclesAndBrokenLinks()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    UnlinkLater escape{root.filePath(QStringLiteral("escape"))};
    UnlinkLater fileLink{root.filePath(QStringLiteral("link-to-secret.txt"))};
    UnlinkLater cycle{root.filePath(QStringLiteral("cycle"))};
    UnlinkLater alias{root.filePath(QStringLiteral("alias-notes"))};
    UnlinkLater broken{root.filePath(QStringLiteral("broken"))};

    QVERIFY(writeFile(root.filePath(QStringLiteral("Report_2024.TXT"))).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("notes/my_report_2024.md"))).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("资料/说明.txt")), QByteArrayLiteral("正文")).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("资料/other.bin"))).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("keep.txt"))).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("EmptyMatch/inside.txt"))).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("plain.dat")), QByteArrayLiteral("report_2024")).isEmpty());
    QVERIFY(QDir(root.filePath(QStringLiteral("only-dir"))).mkpath(QStringLiteral(".")));
    QVERIFY(writeFile(outside.filePath(QStringLiteral("secret-outside.txt")), QByteArrayLiteral("KEEP"))
                .isEmpty());
    QVERIFY(QFile::link(outside.path(), escape.path));
    QVERIFY(QFile::link(outside.filePath(QStringLiteral("secret-outside.txt")), fileLink.path));
    QVERIFY(QFile::link(root.path(), cycle.path));
    QVERIFY(QFile::link(root.filePath(QStringLiteral("notes")), alias.path));
    QVERIFY(QFile::link(root.filePath(QStringLiteral("missing-target")), broken.path));

    const QVector<tw::Term> terms =
        tw::distillTerms(QStringLiteral("report_2024\n\n说明\nREPORT_2024\n"));
    QCOMPARE(terms.size(), 2);

    std::atomic<bool> stop{false};
    const tw::TreeWalk result = tw::walkTree(root.path(), terms, stop, 0, {});
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(!result.cancelled);
    QCOMPARE(result.files.size(), 8);

    const tw::ListedFile *report = findFile(result, QStringLiteral("Report_2024.TXT"));
    const tw::ListedFile *nested = findFile(result, QStringLiteral("notes/my_report_2024.md"));
    const tw::ListedFile *chinese = findFile(result, QStringLiteral("资料/说明.txt"));
    const tw::ListedFile *plain = findFile(result, QStringLiteral("plain.dat"));
    const tw::ListedFile *link = findFile(result, QStringLiteral("link-to-secret.txt"));
    QVERIFY(report && nested && chinese && plain && link);
    QVERIFY(report->matched);
    QCOMPARE(report->hits, QStringList({QStringLiteral("report_2024")}));
    QVERIFY(nested->matched);
    QCOMPARE(nested->hits, QStringList({QStringLiteral("report_2024")}));
    QVERIFY(chinese->matched);
    QCOMPARE(chinese->hits, QStringList({QStringLiteral("说明")}));
    QVERIFY(!plain->matched);
    QVERIFY(plain->hits.isEmpty());
    QVERIFY(!link->matched);
    QVERIFY(!link->absolutePath.contains(QStringLiteral("secret-outside")));

    for (const tw::ListedFile &file : result.files) {
        QVERIFY(!file.relativePath.contains(QStringLiteral("secret-outside")));
        QVERIFY(file.relativePath != QStringLiteral("only-dir"));
        QVERIFY(file.relativePath != QStringLiteral("cycle"));
        QVERIFY(file.relativePath != QStringLiteral("escape"));
        QVERIFY(file.relativePath != QStringLiteral("alias-notes"));
        QVERIFY(file.relativePath != QStringLiteral("broken"));
    }

    const QString issues = issueText(result);
    QVERIFY2(issues.contains(QStringLiteral("符号链接指向起始目录之外，已跳过")), qPrintable(issues));
    QVERIFY2(issues.contains(QStringLiteral("符号链接形成循环，已跳过")), qPrintable(issues));
    QVERIFY2(issues.contains(QStringLiteral("目录符号链接指向目录内部，未跟随")), qPrintable(issues));
    QVERIFY2(issues.contains(QStringLiteral("无法读取符号链接目标")), qPrintable(issues));
    QCOMPARE(result.issues.size(), 4);

    std::atomic<bool> idle{false};
    const tw::TreeWalk relative =
        tw::walkTree(QStringLiteral("relative/path"), terms, idle, 0, {});
    QVERIFY(relative.error.contains(QStringLiteral("绝对路径")));
    QVERIFY(relative.files.isEmpty());
    const tw::TreeWalk missing =
        tw::walkTree(root.filePath(QStringLiteral("no-such")), terms, idle, 0, {});
    QVERIFY(missing.error.contains(QStringLiteral("目录不存在")));
    const tw::TreeWalk slash = tw::walkTree(QStringLiteral("/"), terms, idle, 0, {});
    QVERIFY(slash.error.contains(QStringLiteral("根目录")));
    QVERIFY(slash.files.isEmpty());
}

void HighFilterTests::unreadableDirectoryIsAnIssue()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QString usable;
    QVERIFY(tw::rootRefusal(root.path(), &usable).isEmpty());
    const QString hidden = usable + QStringLiteral("/hidden");
    QVERIFY(writeFile(hidden + QStringLiteral("/hidden.txt")).isEmpty());
    const QFile::Permissions original = QFileInfo(hidden).permissions();
    RestoreMode restore{hidden, original};
    QVERIFY(QFile::setPermissions(hidden, QFileDevice::ExeOwner));

    std::atomic<bool> cancel{false};
    const tw::TreeWalk result =
        tw::walkTree(usable, tw::distillTerms(QStringLiteral("hidden")), cancel, 0, {});
    QVERIFY(result.error.isEmpty());
    QVERIFY(findFile(result, QStringLiteral("hidden/hidden.txt")) == nullptr);
    bool saw = false;
    for (const tw::TreeIssue &issue : result.issues) {
        if (issue.path == hidden && issue.reason.contains(QStringLiteral("无法读取目录")))
            saw = true;
    }
    QVERIFY2(saw, qPrintable(issueText(result)));
}

void HighFilterTests::removalRefusesDirectoriesSymlinksAndOutside()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString stolen = outside.filePath(QStringLiteral("stolen.txt"));
    QVERIFY(writeFile(stolen, QByteArrayLiteral("KEEP")).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("keep.txt")), QByteArrayLiteral("stay")).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("gone.txt"))).isEmpty());
    const QString linkPath = root.filePath(QStringLiteral("link.txt"));
    QVERIFY(QFile::link(stolen, linkPath));
    const QString subLink = root.filePath(QStringLiteral("sub"));
    QVERIFY(QFile::link(outside.path(), subLink));
    UnlinkLater dropLink{linkPath};
    UnlinkLater dropSub{subLink};

    std::atomic<bool> cancel{false};
    const tw::RemovalBatch removed =
        tw::removeListed(root.path(), {root.filePath(QStringLiteral("gone.txt"))}, cancel, 0, {});
    QCOMPARE(removed.items.size(), 1);
    QVERIFY(removed.items.at(0).removed);
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("gone.txt"))));
    QVERIFY(QFileInfo(root.path()).isDir());

    const QString dotdot = root.path() + QStringLiteral("/sub/../keep.txt");
    const QStringList rejected = {root.path(), stolen, root.filePath(QStringLiteral("sub/stolen.txt")),
                                  linkPath, dotdot};
    const tw::RemovalBatch guarded = tw::removeListed(root.path(), rejected, cancel, 0, {});
    QCOMPARE(guarded.items.size(), 5);
    QVERIFY(!guarded.items.at(0).removed);
    QVERIFY(guarded.items.at(0).error.contains(QStringLiteral("拒绝删除目录")));
    QVERIFY(!guarded.items.at(1).removed);
    QVERIFY(guarded.items.at(1).error.contains(QStringLiteral("路径不在所选目录内")));
    QVERIFY(!guarded.items.at(2).removed);
    QVERIFY(guarded.items.at(2).error.contains(QStringLiteral("符号链接")));
    QVERIFY(guarded.items.at(3).removed);
    QVERIFY(!guarded.items.at(4).removed);
    QVERIFY(guarded.items.at(4).error.contains(QStringLiteral("路径不在所选目录内")));

    QVERIFY(QFileInfo(root.path()).isDir());
    QVERIFY(QFileInfo::exists(stolen));
    QFile stolenFile(stolen);
    QVERIFY(stolenFile.open(QIODevice::ReadOnly));
    QCOMPARE(stolenFile.readAll(), QByteArrayLiteral("KEEP"));
    QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("keep.txt"))));
    QVERIFY(!QFileInfo::exists(linkPath));
    QVERIFY(QFileInfo(subLink).isSymLink());

    const QString precious = outside.filePath(QStringLiteral("precious.txt"));
    QVERIFY(writeFile(precious, QByteArrayLiteral("KEEP")).isEmpty());
    const tw::RemovalBatch rootDelete = tw::removeListed(QStringLiteral("/"), {precious}, cancel, 0, {});
    QVERIFY(rootDelete.error.contains(QStringLiteral("根目录")));
    QVERIFY(rootDelete.items.isEmpty());
    QVERIFY(QFileInfo::exists(precious));
}

void HighFilterTests::pageSummarizesFuzzyMatchAndSearch()
{
    Harness env;
    const QString opened = openHarness(&env);
    QVERIFY2(opened.isEmpty(), qPrintable(opened));

    const QString root = env.temp.filePath(QStringLiteral("scan"));
    QVERIFY(writeFile(root + QStringLiteral("/Report_2024.TXT")).isEmpty());
    QVERIFY(writeFile(root + QStringLiteral("/notes/my_report_2024.md")).isEmpty());
    QVERIFY(writeFile(root + QStringLiteral("/资料/说明.txt")).isEmpty());
    QVERIFY(writeFile(root + QStringLiteral("/资料/other.bin")).isEmpty());
    QVERIFY(writeFile(root + QStringLiteral("/keep.txt")).isEmpty());

    auto *terms = env.page->findChild<QPlainTextEdit *>(QStringLiteral("highFilter.terms"));
    auto *directory = env.page->findChild<QLineEdit *>(QStringLiteral("highFilter.directory"));
    auto *start = env.page->findChild<QPushButton *>(QStringLiteral("highFilter.start"));
    auto *matched = env.page->findChild<QLabel *>(QStringLiteral("highFilter.matchedCount"));
    auto *unmatched = env.page->findChild<QLabel *>(QStringLiteral("highFilter.unmatchedCount"));
    auto *total = env.page->findChild<QLabel *>(QStringLiteral("highFilter.totalCount"));
    auto *termCount = env.page->findChild<QLabel *>(QStringLiteral("highFilter.termCount"));
    auto *table = env.page->findChild<QTableView *>(QStringLiteral("highFilter.results"));
    auto *search = env.page->findChild<QLineEdit *>(QStringLiteral("highFilter.search"));
    auto *mode = env.page->findChild<QComboBox *>(QStringLiteral("highFilter.viewMode"));
    auto *rule = env.page->findChild<tw::ElidedLabel *>(QStringLiteral("highFilter.rule"));
    QVERIFY(terms && directory && start && matched && unmatched && total && termCount && table && search
            && mode && rule);
    QVERIFY(rule->fullText().contains(QStringLiteral("不区分大小写")));
    QVERIFY(rule->fullText().contains(QStringLiteral("子串")));
    QVERIFY(rule->fullText().contains(QStringLiteral("相对路径")));
    QVERIFY(rule->fullText().contains(QStringLiteral("第二次确认")));
    QVERIFY(rule->fullText().contains(QStringLiteral("符号链接")));

    terms->setPlainText(QStringLiteral("report_2024\n\n说明\nREPORT_2024\n"));
    QCOMPARE(termCount->text(), QStringLiteral("已识别 2 个关键词"));

    directory->setText(QStringLiteral("/"));
    start->click();
    QVERIFY(!env.page->isBusy());
    QVERIFY(!env.platform->gate()->isLocked());
    QCOMPARE(env.platform->status()->current(QStringLiteral("filter-unique-high")).state,
             tw::StatusState::Error);
    QVERIFY(env.platform->status()
                ->current(QStringLiteral("filter-unique-high"))
                .text.contains(QStringLiteral("根目录")));

    directory->setText(QStringLiteral("relative/not-absolute"));
    start->click();
    QVERIFY(!env.page->isBusy());
    QVERIFY(!env.platform->gate()->isLocked());
    QCOMPARE(env.platform->status()->current(QStringLiteral("filter-unique-high")).state,
             tw::StatusState::Error);

    directory->setText(root);
    start->click();
    QVERIFY(env.page->isBusy());
    QVERIFY(env.platform->gate()->isLocked());
    waitIdle(env.page);
    QVERIFY(!env.platform->gate()->isLocked());
    QCOMPARE(matched->text(), QStringLiteral("3"));
    QCOMPARE(unmatched->text(), QStringLiteral("2"));
    QCOMPARE(total->text(), QStringLiteral("5"));
    QCOMPARE(env.platform->status()->current(QStringLiteral("filter-unique-high")).state,
             tw::StatusState::Success);

    const int reportRow = rowWithPath(table, QStringLiteral("Report_2024.TXT"));
    const int nestedRow = rowWithPath(table, QStringLiteral("notes/my_report_2024.md"));
    const int chineseRow = rowWithPath(table, QStringLiteral("资料/说明.txt"));
    const int keepRow = rowWithPath(table, QStringLiteral("keep.txt"));
    QVERIFY(reportRow >= 0 && nestedRow >= 0 && chineseRow >= 0 && keepRow >= 0);
    QCOMPARE(cellText(table, reportRow, 1), QStringLiteral("匹配"));
    QCOMPARE(cellText(table, reportRow, 2), QStringLiteral("report_2024"));
    QCOMPARE(cellText(table, nestedRow, 2), QStringLiteral("report_2024"));
    QCOMPARE(cellText(table, chineseRow, 2), QStringLiteral("说明"));
    QCOMPARE(cellText(table, keepRow, 1), QStringLiteral("不匹配"));
    QCOMPARE(cellText(table, keepRow, 2), QStringLiteral("无"));

    search->setText(QStringLiteral("KEEP"));
    QCOMPARE(table->model()->rowCount(), 1);
    mode->setCurrentIndex(1);
    QCOMPARE(table->model()->rowCount(), 0);
    search->clear();
    QCOMPARE(table->model()->rowCount(), 3);
    mode->setCurrentIndex(2);
    QCOMPARE(table->model()->rowCount(), 2);
    mode->setCurrentIndex(0);
    QCOMPARE(table->model()->rowCount(), 5);

    const int keepAgain = rowWithPath(table, QStringLiteral("keep.txt"));
    QVERIFY(keepAgain >= 0);
    table->selectRow(keepAgain);
    QApplication::processEvents();
    auto *tree = env.page->findChild<QTreeView *>(QStringLiteral("highFilter.tree"));
    auto *focus = env.page->findChild<tw::ElidedLabel *>(QStringLiteral("highFilter.focusPath"));
    QVERIFY(tree && focus);
    QVERIFY(tree->model()->rowCount() > 0);
    QCOMPARE(focus->fullText(), QStringLiteral("keep.txt"));
    QVERIFY(tree->currentIndex().isValid());
    const QString treeText = tree->model()->data(tree->currentIndex().sibling(tree->currentIndex().row(), 0)).toString();
    QCOMPARE(treeText, QStringLiteral("keep.txt"));

    table->horizontalHeader()->setSortIndicator(0, Qt::DescendingOrder);
    table->model()->sort(0, Qt::DescendingOrder);
    QCOMPARE(table->model()->rowCount(), 5);
    QVERIFY(rowWithPath(table, QStringLiteral("资料/说明.txt")) >= 0);

    auto *detailButton = env.page->findChild<QPushButton *>(QStringLiteral("highFilter.showDetail"));
    auto *fileDialog = env.page->findChild<QWidget *>(QStringLiteral("highFilter.fileDialog"));
    QVERIFY(detailButton && fileDialog);
    QVERIFY(fileDialog->isHidden());
    detailButton->click();
    QVERIFY(QTest::qWaitForWindowExposed(fileDialog));
    QVERIFY(!fileDialog->isHidden());
    QVERIFY(env.page->findChild<QLabel *>(QStringLiteral("highFilter.dialogPath"))
                ->text()
                .endsWith(QStringLiteral("keep.txt")));
    fileDialog->hide();

    QModelIndex notesIndex;
    for (int row = 0; row < tree->model()->rowCount(); ++row) {
        const QModelIndex index = tree->model()->index(row, 0);
        if (index.data(Qt::UserRole).toString() == QStringLiteral("notes"))
            notesIndex = index;
    }
    QVERIFY(notesIndex.isValid());
    QVERIFY(QMetaObject::invokeMethod(env.page, "onTreeDoubleClicked", Q_ARG(QModelIndex, notesIndex)));
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(cellText(table, 0, 0), QStringLiteral("notes/my_report_2024.md"));
    auto *scopeReset = env.page->findChild<QPushButton *>(QStringLiteral("highFilter.scopeReset"));
    QVERIFY(scopeReset && scopeReset->isVisible());
    scopeReset->click();
    QCOMPARE(table->model()->rowCount(), 5);

    QVERIFY(QFileInfo::exists(root + QStringLiteral("/Report_2024.TXT")));
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/keep.txt")));
    QVERIFY(QFileInfo(root).isDir());
}

void HighFilterTests::deletionRequiresSecondConfirmation()
{
    Harness env;
    const QString opened = openHarness(&env);
    QVERIFY2(opened.isEmpty(), qPrintable(opened));
    const QString root = env.temp.filePath(QStringLiteral("scan"));
    const QString matchedPath = root + QStringLiteral("/Report_2024.TXT");
    const QString unmatchedPath = root + QStringLiteral("/keep.txt");
    const QString nestedDir = root + QStringLiteral("/sub");
    QVERIFY(writeFile(matchedPath).isEmpty());
    QVERIFY(writeFile(unmatchedPath).isEmpty());
    QVERIFY(writeFile(nestedDir + QStringLiteral("/my_report_2024.md")).isEmpty());
    QVERIFY(QDir(root + QStringLiteral("/report_2024_dir")).mkpath(QStringLiteral(".")));

    auto *terms = env.page->findChild<QPlainTextEdit *>(QStringLiteral("highFilter.terms"));
    auto *directory = env.page->findChild<QLineEdit *>(QStringLiteral("highFilter.directory"));
    auto *start = env.page->findChild<QPushButton *>(QStringLiteral("highFilter.start"));
    auto *deleteMatched = env.page->findChild<QPushButton *>(QStringLiteral("highFilter.deleteMatched"));
    auto *preview = env.page->findChild<QWidget *>(QStringLiteral("highFilter.preview"));
    auto *previewList = env.page->findChild<QListWidget *>(QStringLiteral("highFilter.previewList"));
    auto *previewCancel = env.page->findChild<QPushButton *>(QStringLiteral("highFilter.previewCancel"));
    auto *previewContinue = env.page->findChild<QPushButton *>(QStringLiteral("highFilter.previewContinue"));
    auto *confirm = env.page->findChild<QPushButton *>(QStringLiteral("highFilter.confirmDelete"));
    QVERIFY(terms && directory && start && deleteMatched && preview && previewList && previewCancel
            && previewContinue && confirm);

    terms->setPlainText(QStringLiteral("report_2024"));
    directory->setText(root);
    start->click();
    waitIdle(env.page);
    QCOMPARE(env.page->findChild<QLabel *>(QStringLiteral("highFilter.matchedCount"))->text(),
             QStringLiteral("2"));
    QVERIFY(preview->isHidden());
    QVERIFY(!confirm->isEnabled());

    deleteMatched->click();
    QVERIFY(!preview->isHidden());
    QCOMPARE(previewList->count(), 2);
    QVERIFY(!confirm->isEnabled());
    QVERIFY(!env.platform->gate()->isLocked());
    QVERIFY(QFileInfo::exists(matchedPath));
    QVERIFY(QFileInfo::exists(nestedDir + QStringLiteral("/my_report_2024.md")));
    QVERIFY(QFileInfo::exists(unmatchedPath));

    previewCancel->click();
    QVERIFY(preview->isHidden());
    QVERIFY(QFileInfo::exists(matchedPath));
    QVERIFY(!env.platform->gate()->isLocked());

    deleteMatched->click();
    previewContinue->click();
    QVERIFY(confirm->isEnabled());
    QVERIFY(!env.page->isBusy());
    QVERIFY(!env.platform->gate()->isLocked());
    QVERIFY(QFileInfo::exists(matchedPath));
    QVERIFY(QFileInfo::exists(unmatchedPath));

    confirm->click();
    QVERIFY(env.page->isBusy());
    QVERIFY(env.platform->gate()->isLocked());
    waitIdle(env.page);
    QVERIFY(!env.platform->gate()->isLocked());
    QVERIFY(!QFileInfo::exists(matchedPath));
    QVERIFY(!QFileInfo::exists(nestedDir + QStringLiteral("/my_report_2024.md")));
    QVERIFY(QFileInfo::exists(unmatchedPath));
    QVERIFY(QFileInfo(nestedDir).isDir());
    QVERIFY(QFileInfo(root).isDir());
    QVERIFY(QFileInfo(root + QStringLiteral("/report_2024_dir")).isDir());
    QCOMPARE(env.page->findChild<QLabel *>(QStringLiteral("highFilter.matchedCount"))->text(),
             QStringLiteral("0"));
    QCOMPARE(env.page->findChild<QLabel *>(QStringLiteral("highFilter.unmatchedCount"))->text(),
             QStringLiteral("1"));

    auto *deleteUnmatched =
        env.page->findChild<QPushButton *>(QStringLiteral("highFilter.deleteUnmatched"));
    QVERIFY(deleteUnmatched);
    deleteUnmatched->click();
    QCOMPARE(previewList->count(), 1);
    QVERIFY(!confirm->isEnabled());
    previewContinue->click();
    QVERIFY(confirm->isEnabled());
    QVERIFY(QFileInfo::exists(unmatchedPath));
    confirm->click();
    waitIdle(env.page);
    QVERIFY(!QFileInfo::exists(unmatchedPath));
    QVERIFY(QFileInfo(root).isDir());
    QVERIFY(QFileInfo(nestedDir).isDir());
    QCOMPARE(env.page->findChild<QLabel *>(QStringLiteral("highFilter.unmatchedCount"))->text(),
             QStringLiteral("0"));
    QCOMPARE(env.page->findChild<QLabel *>(QStringLiteral("highFilter.totalCount"))->text(),
             QStringLiteral("0"));
}

void HighFilterTests::removalFailureNamesTheReason()
{
    Harness env;
    const QString opened = openHarness(&env);
    QVERIFY2(opened.isEmpty(), qPrintable(opened));
    const QString root = env.temp.filePath(QStringLiteral("scan"));
    const QString blocked = root + QStringLiteral("/a.txt");
    const QString gone = root + QStringLiteral("/b.txt");
    QVERIFY(writeFile(blocked).isEmpty());
    QVERIFY(writeFile(gone).isEmpty());

    auto *terms = env.page->findChild<QPlainTextEdit *>(QStringLiteral("highFilter.terms"));
    auto *directory = env.page->findChild<QLineEdit *>(QStringLiteral("highFilter.directory"));
    terms->setPlainText(QStringLiteral("txt"));
    directory->setText(root);
    env.page->findChild<QPushButton *>(QStringLiteral("highFilter.start"))->click();
    waitIdle(env.page);

    QVERIFY(QFile::remove(blocked));
    QVERIFY(QDir().mkpath(blocked));
    env.page->findChild<QPushButton *>(QStringLiteral("highFilter.deleteMatched"))->click();
    env.page->findChild<QPushButton *>(QStringLiteral("highFilter.previewContinue"))->click();
    env.page->findChild<QPushButton *>(QStringLiteral("highFilter.confirmDelete"))->click();
    waitIdle(env.page);

    QVERIFY(QFileInfo(blocked).isDir());
    QVERIFY(!QFileInfo::exists(gone));
    QVERIFY(QFileInfo(root).isDir());
    auto *problems = env.page->findChild<QTableView *>(QStringLiteral("highFilter.issues"));
    QVERIFY(problems);
    QVERIFY(problems->model()->rowCount() >= 1);
    bool sawReason = false;
    for (int row = 0; row < problems->model()->rowCount(); ++row) {
        const QString reason = problems->model()->index(row, 1).data().toString();
        if (reason.contains(QStringLiteral("拒绝删除目录")))
            sawReason = true;
    }
    QVERIFY(sawReason);
    QCOMPARE(env.platform->status()->current(QStringLiteral("filter-unique-high")).state,
             tw::StatusState::Error);
    QVERIFY(env.page->findChild<QLabel *>(QStringLiteral("highFilter.detail"))
                ->text()
                .contains(QStringLiteral("失败")));
    QVERIFY(!env.platform->gate()->isLocked());
}

void HighFilterTests::cancelWalkAndRemovalTouchNothing()
{
    Harness env;
    const QString opened = openHarness(&env);
    QVERIFY2(opened.isEmpty(), qPrintable(opened));
    const QString root = env.temp.filePath(QStringLiteral("scan"));
    QStringList paths;
    for (int i = 0; i < 8; ++i) {
        const QString path = root + QStringLiteral("/f%1.txt").arg(i);
        paths.append(path);
        QVERIFY(writeFile(path).isEmpty());
    }

    auto *terms = env.page->findChild<QPlainTextEdit *>(QStringLiteral("highFilter.terms"));
    auto *directory = env.page->findChild<QLineEdit *>(QStringLiteral("highFilter.directory"));
    auto *start = env.page->findChild<QPushButton *>(QStringLiteral("highFilter.start"));
    auto *cancel = env.page->findChild<QPushButton *>(QStringLiteral("highFilter.cancel"));
    terms->setPlainText(QStringLiteral("txt"));
    directory->setText(root);

    env.page->setPaceMs(400);
    start->click();
    QVERIFY(env.page->isBusy());
    QVERIFY(env.platform->gate()->isLocked());
    QVERIFY(cancel->isEnabled());
    cancel->click();
    waitIdle(env.page);
    QVERIFY(!env.platform->gate()->isLocked());
    QCOMPARE(env.page->findChild<QLabel *>(QStringLiteral("highFilter.totalCount"))->text(),
             QStringLiteral("0"));
    QVERIFY(env.platform->status()
                ->current(QStringLiteral("filter-unique-high"))
                .text.contains(QStringLiteral("已取消筛选")));
    for (const QString &path : paths)
        QVERIFY(QFileInfo::exists(path));

    env.page->setPaceMs(0);
    start->click();
    waitIdle(env.page);
    QCOMPARE(env.page->findChild<QLabel *>(QStringLiteral("highFilter.matchedCount"))->text(),
             QStringLiteral("8"));

    env.page->setPaceMs(400);
    env.page->findChild<QPushButton *>(QStringLiteral("highFilter.deleteMatched"))->click();
    env.page->findChild<QPushButton *>(QStringLiteral("highFilter.previewContinue"))->click();
    env.page->findChild<QPushButton *>(QStringLiteral("highFilter.confirmDelete"))->click();
    QVERIFY(env.page->isBusy());
    QVERIFY(env.platform->gate()->isLocked());
    cancel->click();
    waitIdle(env.page);
    QVERIFY(!env.platform->gate()->isLocked());
    QVERIFY(env.platform->status()
                ->current(QStringLiteral("filter-unique-high"))
                .text.contains(QStringLiteral("已取消删除")));
    for (const QString &path : paths)
        QVERIFY(QFileInfo::exists(path));
    QVERIFY(QFileInfo(root).isDir());
}

void HighFilterTests::importUtf8DedupesAndRejectsInvalid()
{
    Harness env;
    const QString opened = openHarness(&env);
    QVERIFY2(opened.isEmpty(), qPrintable(opened));
    auto *terms = env.page->findChild<QPlainTextEdit *>(QStringLiteral("highFilter.terms"));
    auto *count = env.page->findChild<QLabel *>(QStringLiteral("highFilter.termCount"));
    terms->setPlainText(QStringLiteral("保留"));

    const QString badPath = env.temp.filePath(QStringLiteral("bad.txt"));
    QFile bad(badPath);
    QVERIFY(bad.open(QIODevice::WriteOnly));
    bad.write("\xff\xfe not utf8");
    bad.close();
    QString error;
    QVERIFY(!env.page->loadTermsFromFile(badPath, &error));
    QVERIFY(error.contains(QStringLiteral("UTF-8")));
    QCOMPARE(terms->toPlainText(), QStringLiteral("保留"));

    const QString goodPath = env.temp.filePath(QStringLiteral("names.txt"));
    QFile good(goodPath);
    QVERIFY(good.open(QIODevice::WriteOnly));
    good.write("\xEF\xBB\xBF"
               "Alpha\n\nalpha\r\n  B  \nb\n说明\n");
    good.close();
    QVERIFY2(env.page->loadTermsFromFile(goodPath, &error), qPrintable(error));
    QCOMPARE(count->text(), QStringLiteral("已识别 3 个关键词"));
    const QVector<tw::Term> distilled = tw::distillTerms(terms->toPlainText());
    QCOMPARE(distilled.size(), 3);
    QCOMPARE(distilled.at(0).shown, QStringLiteral("Alpha"));
    QCOMPARE(distilled.at(1).shown, QStringLiteral("B"));
    QCOMPARE(distilled.at(2).shown, QStringLiteral("说明"));
}

void HighFilterTests::stateStaysInsideDataDir()
{
    Harness env;
    const QString opened = openHarness(&env);
    QVERIFY2(opened.isEmpty(), qPrintable(opened));
    const QString terms = QStringLiteral("名单甲\n名单乙");
    const QString directory = env.temp.filePath(QStringLiteral("scan"));
    QVERIFY(QDir().mkpath(directory));
    env.page->findChild<QPlainTextEdit *>(QStringLiteral("highFilter.terms"))->setPlainText(terms);
    env.page->findChild<QLineEdit *>(QStringLiteral("highFilter.directory"))->setText(directory);

    const QString dataDir = env.platform->paths().toolDataDir(QStringLiteral("filter-unique-high"));
    const QString statePath = QDir(dataDir).filePath(QStringLiteral("high-filter-state.json"));
    QVERIFY(statePath.contains(QStringLiteral("/tools/filter-unique-high/high-filter-state.json")));
    QVERIFY(statePath.startsWith(env.platform->paths().root()));
    QFile stateFile(statePath);
    QVERIFY(stateFile.open(QIODevice::ReadOnly));
    const QJsonObject state = QJsonDocument::fromJson(stateFile.readAll()).object();
    QCOMPARE(state.value(QStringLiteral("terms")).toString(), terms);
    QCOMPARE(state.value(QStringLiteral("directory")).toString(), directory);

    tw::HighFilterTool again;
    tw::ToolContext context;
    context.info = again.info();
    context.services = env.platform->servicesFor(context.info.id);
    QString error;
    QVERIFY2(again.initialize(context, &error), qPrintable(error));
    const QVector<tw::ToolPage> pages = again.createPages();
    QCOMPARE(pages.size(), 1);
    auto *page = qobject_cast<tw::HighFilterPage *>(pages.at(0).widget);
    QVERIFY(page);
    QCOMPARE(page->findChild<QPlainTextEdit *>(QStringLiteral("highFilter.terms"))->toPlainText(), terms);
    QCOMPARE(page->findChild<QLineEdit *>(QStringLiteral("highFilter.directory"))->text(), directory);
    delete page;
}

void HighFilterTests::resultTableStaysVisibleInTypicalWindow()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    tw::ToolRegistry registry;
    QString error;
    QVERIFY2(registry.add(std::make_unique<tw::HighFilterTool>(), &error), qPrintable(error));
    tw::MainWindow window(std::make_unique<tw::PlatformServices>(
                              tw::DataPaths(dir.filePath(QStringLiteral("data")))),
                          std::move(registry));
    window.resize(1180, 760);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.selectToolById(QStringLiteral("filter-unique-high")));
    QVERIFY(window.currentView() != nullptr);
    QWidget *page = window.currentView()->pageWidget(0);
    QVERIFY(page);

    bool sawTitle = false;
    for (int i = 0; i < window.toolRail()->count(); ++i) {
        if (window.toolRail()->item(i)->text() == QStringLiteral("筛选去重high"))
            sawTitle = true;
    }
    QVERIFY(sawTitle);

    auto *table = page->findChild<QTableView *>(QStringLiteral("highFilter.results"));
    auto *tree = page->findChild<QTreeView *>(QStringLiteral("highFilter.tree"));
    auto *issues = page->findChild<QTableView *>(QStringLiteral("highFilter.issues"));
    auto *detail = page->findChild<QWidget *>(QStringLiteral("highFilter.detailPane"));
    auto *terms = page->findChild<QPlainTextEdit *>(QStringLiteral("highFilter.terms"));
    auto *rule = page->findChild<tw::ElidedLabel *>(QStringLiteral("highFilter.rule"));
    auto *workSplit = page->findChild<QSplitter *>(QStringLiteral("highFilter.workSplit"));
    QVERIFY(table && tree && issues && detail && terms && rule && workSplit);
    QApplication::processEvents();

    auto overlap = [&window](QWidget *widget) -> QString {
        if (!widget->isVisibleTo(&window))
            return widget->objectName() + QStringLiteral(" 不可见");
        const QPoint origin = widget->mapTo(&window, QPoint(0, 0));
        if (origin.y() < 48)
            return QStringLiteral("%1 y=%2").arg(widget->objectName()).arg(origin.y());
        if (origin.y() + widget->height() > window.height() - 28)
            return QStringLiteral("%1 y=%2 h=%3 window=%4")
                .arg(widget->objectName())
                .arg(origin.y())
                .arg(widget->height())
                .arg(window.height());
        if (origin.x() < 196 || origin.x() + widget->width() > window.width())
            return QStringLiteral("%1 x=%2 w=%3").arg(widget->objectName()).arg(origin.x()).arg(widget->width());
        return {};
    };
    QVERIFY2(overlap(table).isEmpty(), qPrintable(overlap(table)));
    QVERIFY2(overlap(tree).isEmpty(), qPrintable(overlap(tree)));
    QVERIFY2(overlap(issues).isEmpty(), qPrintable(overlap(issues)));
    QVERIFY2(overlap(detail).isEmpty(), qPrintable(overlap(detail)));

    QVERIFY2(table->height() >= 160, qPrintable(QString::number(table->height())));
    QVERIFY(table->width() >= 480);
    QVERIFY(tree->height() >= 160);
    QVERIFY(issues->height() >= 48);
    const QPoint origin = table->mapTo(&window, QPoint(0, 0));
    const QPoint treePos = tree->mapTo(&window, QPoint(0, 0));
    const QPoint detailPos = detail->mapTo(&window, QPoint(0, 0));
    const QPoint issuePos = issues->mapTo(&window, QPoint(0, 0));
    QVERIFY(treePos.x() < origin.x());
    QVERIFY(detailPos.x() >= origin.x() + table->width() - 4);
    QVERIFY(issuePos.y() >= origin.y() + table->height() - 4);
    QVERIFY(rule->sizeHint().height() <= rule->fontMetrics().height() + 8);
    const QPoint rulePos = rule->mapTo(&window, QPoint(0, 0));
    QVERIFY(rulePos.y() + rule->height() <= origin.y() + 2);
    QVERIFY(workSplit->orientation() == Qt::Horizontal);
    QCOMPARE(workSplit->count(), 3);

    const QPixmap emptyShot = window.grab();
    emptyShot.save(QStringLiteral("/tmp/filter-unique-high-empty-1180x760.png"));
    QDir().mkpath(QStringLiteral("/opt/cursor/artifacts"));
    emptyShot.save(QStringLiteral("/opt/cursor/artifacts/filter-unique-high-empty-1180x760.png"));

    const QString root = dir.filePath(QStringLiteral("scan"));
    QVERIFY(writeFile(root + QStringLiteral("/Report_2024.TXT")).isEmpty());
    QVERIFY(writeFile(root + QStringLiteral("/notes/my_report_2024.md")).isEmpty());
    QVERIFY(writeFile(root + QStringLiteral("/资料/说明.txt")).isEmpty());
    QVERIFY(writeFile(root + QStringLiteral("/keep.txt")).isEmpty());
    page->findChild<QPlainTextEdit *>(QStringLiteral("highFilter.terms"))
        ->setPlainText(QStringLiteral("report_2024\n说明"));
    page->findChild<QLineEdit *>(QStringLiteral("highFilter.directory"))->setText(root);
    page->findChild<QPushButton *>(QStringLiteral("highFilter.start"))->click();
    auto *board = qobject_cast<tw::HighFilterPage *>(page);
    QVERIFY(board);
    QTRY_VERIFY_WITH_TIMEOUT(!board->isBusy(), 8000);
    const int nested = rowWithPath(table, QStringLiteral("notes/my_report_2024.md"));
    QVERIFY(nested >= 0);
    table->selectRow(nested);
    QApplication::processEvents();
    QVERIFY2(overlap(table).isEmpty(), qPrintable(overlap(table)));
    QVERIFY2(overlap(tree).isEmpty(), qPrintable(overlap(tree)));
    QVERIFY2(overlap(issues).isEmpty(), qPrintable(overlap(issues)));
    QVERIFY(table->height() >= 160);
    QVERIFY(table->width() >= 480);
    QVERIFY(tree->model()->rowCount() > 0);

    const QPixmap scannedShot = window.grab();
    scannedShot.save(QStringLiteral("/tmp/filter-unique-high-scanned-1180x760.png"));
    scannedShot.save(QStringLiteral("/opt/cursor/artifacts/filter-unique-high-scanned-1180x760.png"));
}

QTEST_MAIN(HighFilterTests)
#include "high_filter_test.moc"
