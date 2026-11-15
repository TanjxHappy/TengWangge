#include "tools/filter_unique/filter_unique_scan.h"
#include "tools/filter_unique/filter_unique_tool.h"

#include "core/data_paths.h"
#include "core/platform_services.h"
#include "shell/tool_registry.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#include <atomic>
#include <memory>

#include <unistd.h>

using namespace tw;

namespace {

int forceOffscreen()
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    return 0;
}

const int kForceOffscreen = forceOffscreen();

QString writeFile(const QString &path, const QByteArray &data = QByteArrayLiteral("x"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return file.errorString();
    if (file.write(data) != data.size())
        return QStringLiteral("短写");
    return {};
}

struct RemovePath
{
    QString path;
    ~RemovePath()
    {
        if (!path.isEmpty())
            QFile::remove(path);
    }
};

struct Env
{
    QTemporaryDir temp;
    std::unique_ptr<PlatformServices> platform;
    std::unique_ptr<FilterUniqueTool> tool;
    QWidget host;
    FilterUniquePage *page = nullptr;
};

QString openTool(Env *env)
{
    if (!env->temp.isValid())
        return QStringLiteral("临时目录不可用");
    env->platform = std::make_unique<PlatformServices>(DataPaths(env->temp.filePath(QStringLiteral("data"))));
    env->tool = std::make_unique<FilterUniqueTool>();
    ToolContext context;
    context.info = env->tool->info();
    context.services = env->platform->servicesFor(context.info.id);
    QString error;
    if (!env->tool->initialize(context, &error))
        return error;
    const QVector<ToolPage> pages = env->tool->createPages();
    if (pages.size() != 1 || !pages.at(0).widget)
        return QStringLiteral("页面创建失败");
    pages.at(0).widget->setParent(&env->host);
    env->page = qobject_cast<FilterUniquePage *>(pages.at(0).widget);
    if (!env->page)
        return QStringLiteral("页面类型不符");
    env->host.resize(1100, 760);
    env->host.show();
    if (!QTest::qWaitForWindowExposed(&env->host))
        return QStringLiteral("窗口未能显示");
    return {};
}

QString cellText(QTableView *table, int row, int column)
{
    return table->model()->index(row, column).data(Qt::DisplayRole).toString();
}

int rowWithPath(QTableView *table, const QString &path)
{
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        if (cellText(table, row, 0) == path)
            return row;
    }
    return -1;
}

} // namespace

class FilterUniqueTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { Q_UNUSED(kForceOffscreen); }

    void identifierAndRuleAreStable();
    void parseKeywordsTrimsAndDedupes();
    void fuzzyMatchIsCaseInsensitiveSubstring();
    void scanSummarizesMatchAndSkipsUnsafeLinks();
    void deleteRefusesDirectoriesOutsidePathsAndSymlinkEscape();
    void pageShowsSummaryFuzzyMatchAndSearch();
    void deleteWaitsForSecondConfirm();
    void deleteFailureIsListed();
    void cancelScanAndDeleteLeaveFilesUntouched();
    void sessionStaysInsideDataDir();
};

void FilterUniqueTests::identifierAndRuleAreStable()
{
    FilterUniqueTool tool;
    const ToolInfo info = tool.info();
    QCOMPARE(info.id, QStringLiteral("filter-unique"));
    QVERIFY(!info.id.contains(QLatin1Char('_')));
    QCOMPARE(info.title, QStringLiteral("筛选去重"));
    QVERIFY(!info.title.isEmpty());

    PlatformServices blocked(DataPaths(QStringLiteral("/")));
    ToolContext context;
    context.info = info;
    context.services = blocked.servicesFor(info.id);
    QString error;
    QVERIFY(!tool.initialize(context, &error));
    QVERIFY(!error.isEmpty());

    Env env;
    const QString opened = openTool(&env);
    QVERIFY2(opened.isEmpty(), qPrintable(opened));
    auto *rule = env.page->findChild<QLabel *>(QStringLiteral("filterUnique.rule"));
    QVERIFY(rule);
    QVERIFY(rule->text().contains(QStringLiteral("不区分大小写")));
    QVERIFY(rule->text().contains(QStringLiteral("子串")));
    QVERIFY(rule->text().contains(QStringLiteral("相对路径")));
    QVERIFY(rule->text().contains(QStringLiteral("第二次确认")));
    QVERIFY(rule->text().contains(QStringLiteral("符号链接")));
}

void FilterUniqueTests::parseKeywordsTrimsAndDedupes()
{
    const QStringList keywords = parseKeywords(QStringLiteral("  Alpha  \n\nAlpha\r\nb\r\n  B  \n   \n说明\n"));
    QCOMPARE(keywords, QStringList({QStringLiteral("Alpha"), QStringLiteral("b"), QStringLiteral("说明")}));
    QCOMPARE(parseKeywords(QStringLiteral(" \n\t")), QStringList());
}

void FilterUniqueTests::fuzzyMatchIsCaseInsensitiveSubstring()
{
    const QStringList keywords = {QStringLiteral("report_2024"), QStringLiteral("说明")};
    const QStringList report = matchedKeywords(keywords, QStringLiteral("notes/my_report_2024.md"),
                                               QStringLiteral("my_report_2024.md"));
    QCOMPARE(report, QStringList({QStringLiteral("report_2024")}));

    const QStringList chinese = matchedKeywords(keywords, QStringLiteral("资料/说明.txt"),
                                                QStringLiteral("说明.txt"));
    QCOMPARE(chinese, QStringList({QStringLiteral("说明")}));

    QVERIFY(matchedKeywords(keywords, QStringLiteral("keep.txt"), QStringLiteral("keep.txt")).isEmpty());
    // 所选目录本身的名字不参与比较：这里只传入相对路径。
    QVERIFY(matchedKeywords({QStringLiteral("SpecialRoot")}, QStringLiteral("a.txt"),
                            QStringLiteral("a.txt"))
                .isEmpty());
    // 点号和其他字符按字面子串处理，不当成通配符。
    QVERIFY(matchedKeywords({QStringLiteral("report.txt")}, QStringLiteral("report_2024.TXT"),
                            QStringLiteral("report_2024.TXT"))
                .isEmpty());
}

void FilterUniqueTests::scanSummarizesMatchAndSkipsUnsafeLinks()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    RemovePath escapeGuard{root.filePath(QStringLiteral("escape"))};
    RemovePath fileLinkGuard{root.filePath(QStringLiteral("link-to-secret.txt"))};
    RemovePath cycleGuard{root.filePath(QStringLiteral("cycle"))};
    RemovePath aliasGuard{root.filePath(QStringLiteral("alias-notes"))};
    RemovePath brokenGuard{root.filePath(QStringLiteral("broken"))};

    QVERIFY(writeFile(root.filePath(QStringLiteral("Report_2024.TXT"))).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("notes/my_report_2024.md"))).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("资料/说明.txt")), QByteArrayLiteral("正文")).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("资料/other.bin"))).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("keep.txt"))).isEmpty());
    QVERIFY(writeFile(root.filePath(QStringLiteral("EmptyMatch/inside.txt"))).isEmpty());
    QVERIFY(QDir(root.filePath(QStringLiteral("only-dir"))).mkpath(QStringLiteral(".")));
    QVERIFY(writeFile(outside.filePath(QStringLiteral("secret-outside.txt")), QByteArrayLiteral("KEEP")).isEmpty());
    QVERIFY(QFile::link(outside.path(), escapeGuard.path));
    QVERIFY(QFile::link(outside.filePath(QStringLiteral("secret-outside.txt")), fileLinkGuard.path));
    QVERIFY(QFile::link(root.path(), cycleGuard.path));
    QVERIFY(QFile::link(root.filePath(QStringLiteral("notes")), aliasGuard.path));
    QVERIFY(QFile::link(root.filePath(QStringLiteral("missing-target")), brokenGuard.path));

    std::atomic<bool> cancel{false};
    const QStringList keywords = parseKeywords(QStringLiteral("report_2024\n\n说明\nREPORT_2024\n"));
    QCOMPARE(keywords.size(), 2);
    const FilterScanResult result = scanDirectory(root.path(), keywords, cancel, {});
    QVERIFY(result.error.isEmpty());
    QVERIFY(!result.cancelled);
    QCOMPARE(result.files.size(), 7); // 6 个普通文件 + 1 个指向文件的符号链接

    int matched = 0;
    bool sawOutsideName = false;
    bool sawDirectoryAsFile = false;
    bool sawLink = false;
    for (const FilterFileRow &row : result.files) {
        if (row.matched)
            ++matched;
        if (row.relativePath.contains(QStringLiteral("secret-outside"))
            || row.fileName.contains(QStringLiteral("secret-outside")))
            sawOutsideName = true;
        if (row.relativePath == QStringLiteral("only-dir") || row.relativePath == QStringLiteral("EmptyMatch")
            || row.relativePath == QStringLiteral("cycle") || row.relativePath == QStringLiteral("escape"))
            sawDirectoryAsFile = true;
        if (row.relativePath == QStringLiteral("link-to-secret.txt")) {
            sawLink = true;
            QCOMPARE(row.matchedTerms, QStringList());
        }
        if (row.relativePath == QStringLiteral("Report_2024.TXT")
            || row.relativePath == QStringLiteral("notes/my_report_2024.md")) {
            QVERIFY(row.matched);
            QCOMPARE(row.matchedTerms, QStringList({QStringLiteral("report_2024")}));
        }
        if (row.relativePath == QStringLiteral("资料/说明.txt")) {
            QVERIFY(row.matched);
            QCOMPARE(row.matchedTerms, QStringList({QStringLiteral("说明")}));
        }
    }
    QCOMPARE(matched, 3);
    QVERIFY(!sawOutsideName);
    QVERIFY(!sawDirectoryAsFile);
    QVERIFY(sawLink);

    auto reasonContains = [&result](const QString &fragment) {
        for (const FilterProblem &problem : result.problems) {
            if (problem.reason.contains(fragment))
                return true;
        }
        return false;
    };
    QVERIFY(reasonContains(QStringLiteral("符号链接指向起始目录之外，已跳过")));
    QVERIFY(reasonContains(QStringLiteral("符号链接形成循环，已跳过")));
    QVERIFY(reasonContains(QStringLiteral("目录符号链接指向目录内部，未跟随")));
    QVERIFY(reasonContains(QStringLiteral("无法读取符号链接目标")));

    std::atomic<bool> stop{false};
    const FilterScanResult relative = scanDirectory(QStringLiteral("relative/path"), keywords, stop, {});
    QVERIFY(!relative.error.isEmpty());
    const FilterScanResult missing = scanDirectory(root.filePath(QStringLiteral("no-such")), keywords, stop, {});
    QVERIFY(missing.error.contains(QStringLiteral("目录不存在")));
}

void FilterUniqueTests::deleteRefusesDirectoriesOutsidePathsAndSymlinkEscape()
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
    RemovePath linkGuard{linkPath};
    const QString subLink = root.filePath(QStringLiteral("sub"));
    QVERIFY(QFile::link(outside.path(), subLink));
    RemovePath subGuard{subLink};

    std::atomic<bool> cancel{false};
    const FilterDeleteResult removed = deleteListedFiles(
        root.path(), {root.filePath(QStringLiteral("gone.txt"))}, cancel, {});
    QCOMPARE(removed.items.size(), 1);
    QVERIFY(removed.items.at(0).removed);
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("gone.txt"))));
    QVERIFY(QFileInfo(root.path()).isDir());

    const QStringList rejected = {root.path(), stolen, root.filePath(QStringLiteral("sub/stolen.txt")), linkPath};
    const FilterDeleteResult guarded = deleteListedFiles(root.path(), rejected, cancel, {});
    QCOMPARE(guarded.items.size(), 4);
    QVERIFY(!guarded.items.at(0).removed);
    QVERIFY(guarded.items.at(0).error.contains(QStringLiteral("拒绝删除目录")));
    QVERIFY(!guarded.items.at(1).removed);
    QVERIFY(guarded.items.at(1).error.contains(QStringLiteral("路径不在所选目录内")));
    QVERIFY(!guarded.items.at(2).removed);
    QVERIFY(guarded.items.at(2).error.contains(QStringLiteral("符号链接")));
    QVERIFY(guarded.items.at(3).removed);

    QVERIFY(QFileInfo(root.path()).isDir());
    QVERIFY(QFileInfo::exists(stolen));
    QCOMPARE(QFile(stolen).exists(), true);
    QFile stolenFile(stolen);
    QVERIFY(stolenFile.open(QIODevice::ReadOnly));
    QCOMPARE(stolenFile.readAll(), QByteArrayLiteral("KEEP"));
    QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("keep.txt"))));
    QVERIFY(!QFileInfo::exists(linkPath));
    QVERIFY(QFileInfo(subLink).isSymLink());
}

void FilterUniqueTests::pageShowsSummaryFuzzyMatchAndSearch()
{
    Env env;
    const QString opened = openTool(&env);
    QVERIFY2(opened.isEmpty(), qPrintable(opened));

    const QString root = env.temp.filePath(QStringLiteral("scan"));
    QVERIFY(writeFile(root + QStringLiteral("/Report_2024.TXT")).isEmpty());
    QVERIFY(writeFile(root + QStringLiteral("/notes/my_report_2024.md")).isEmpty());
    QVERIFY(writeFile(root + QStringLiteral("/资料/说明.txt")).isEmpty());
    QVERIFY(writeFile(root + QStringLiteral("/资料/other.bin")).isEmpty());
    QVERIFY(writeFile(root + QStringLiteral("/keep.txt")).isEmpty());

    auto *keywords = env.page->findChild<QPlainTextEdit *>(QStringLiteral("filterUnique.keywords"));
    auto *directory = env.page->findChild<QLineEdit *>(QStringLiteral("filterUnique.directory"));
    auto *start = env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.start"));
    auto *matched = env.page->findChild<QLabel *>(QStringLiteral("filterUnique.matchedCount"));
    auto *unmatched = env.page->findChild<QLabel *>(QStringLiteral("filterUnique.unmatchedCount"));
    auto *total = env.page->findChild<QLabel *>(QStringLiteral("filterUnique.totalCount"));
    auto *keywordCount = env.page->findChild<QLabel *>(QStringLiteral("filterUnique.keywordCount"));
    auto *table = env.page->findChild<QTableView *>(QStringLiteral("filterUnique.results"));
    auto *search = env.page->findChild<QLineEdit *>(QStringLiteral("filterUnique.search"));
    auto *mode = env.page->findChild<QComboBox *>(QStringLiteral("filterUnique.resultMode"));
    QVERIFY(keywords && directory && start && matched && unmatched && total && keywordCount && table && search
            && mode);

    keywords->setPlainText(QStringLiteral("report_2024\n\n说明\nREPORT_2024\n"));
    QCOMPARE(keywordCount->text(), QStringLiteral("已识别 2 个关键词"));
    directory->setText(QStringLiteral("relative/not-absolute"));
    start->click();
    QVERIFY(!env.page->isBusy());
    QVERIFY(!env.platform->gate()->isLocked());
    QCOMPARE(env.platform->status()->current(QStringLiteral("filter-unique")).state, StatusState::Error);

    directory->setText(root);
    QVERIFY(!env.platform->gate()->isLocked());
    start->click();
    QVERIFY(env.page->isBusy());
    QVERIFY(env.platform->gate()->isLocked());
    QTRY_VERIFY_WITH_TIMEOUT(!env.page->isBusy(), 5000);
    QVERIFY(!env.platform->gate()->isLocked());
    QCOMPARE(matched->text(), QStringLiteral("3"));
    QCOMPARE(unmatched->text(), QStringLiteral("2"));
    QCOMPARE(total->text(), QStringLiteral("5"));
    QCOMPARE(env.platform->status()->current(QStringLiteral("filter-unique")).state, StatusState::Success);

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

    QVERIFY(QFileInfo::exists(root + QStringLiteral("/Report_2024.TXT")));
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/keep.txt")));
    QVERIFY(QFileInfo(root).isDir());
}

void FilterUniqueTests::deleteWaitsForSecondConfirm()
{
    Env env;
    const QString opened = openTool(&env);
    QVERIFY2(opened.isEmpty(), qPrintable(opened));
    const QString root = env.temp.filePath(QStringLiteral("scan"));
    const QString matchedPath = root + QStringLiteral("/Report_2024.TXT");
    const QString unmatchedPath = root + QStringLiteral("/keep.txt");
    const QString nestedDir = root + QStringLiteral("/sub");
    QVERIFY(writeFile(matchedPath).isEmpty());
    QVERIFY(writeFile(unmatchedPath).isEmpty());
    QVERIFY(writeFile(nestedDir + QStringLiteral("/my_report_2024.md")).isEmpty());

    auto *keywords = env.page->findChild<QPlainTextEdit *>(QStringLiteral("filterUnique.keywords"));
    auto *directory = env.page->findChild<QLineEdit *>(QStringLiteral("filterUnique.directory"));
    auto *start = env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.start"));
    auto *deleteMatched = env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.deleteMatched"));
    auto *preview = env.page->findChild<QWidget *>(QStringLiteral("filterUnique.preview"));
    auto *previewList = env.page->findChild<QListWidget *>(QStringLiteral("filterUnique.previewList"));
    auto *previewCancel = env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.previewCancel"));
    auto *previewContinue = env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.previewContinue"));
    auto *confirm = env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.confirmDelete"));
    QVERIFY(keywords && directory && start && deleteMatched && preview && previewList && previewCancel
            && previewContinue && confirm);

    keywords->setPlainText(QStringLiteral("report_2024"));
    directory->setText(root);
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(!env.page->isBusy(), 5000);
    QCOMPARE(env.page->findChild<QLabel *>(QStringLiteral("filterUnique.matchedCount"))->text(),
             QStringLiteral("2"));
    QVERIFY(preview->isHidden());
    QVERIFY(!confirm->isEnabled());

    deleteMatched->click();
    QVERIFY(!preview->isHidden());
    QCOMPARE(previewList->count(), 2);
    QVERIFY(!confirm->isEnabled());
    confirm->click();
    QVERIFY(QFileInfo::exists(matchedPath));
    QVERIFY(QFileInfo::exists(nestedDir + QStringLiteral("/my_report_2024.md")));
    QVERIFY(QFileInfo::exists(unmatchedPath));
    QVERIFY(!env.platform->gate()->isLocked());

    previewCancel->click();
    QVERIFY(preview->isHidden());
    QVERIFY(QFileInfo::exists(matchedPath));

    deleteMatched->click();
    previewContinue->click();
    QVERIFY(confirm->isEnabled());
    QVERIFY(QFileInfo::exists(matchedPath));
    QVERIFY(QFileInfo::exists(unmatchedPath));
    confirm->click();
    QVERIFY(env.page->isBusy());
    QVERIFY(env.platform->gate()->isLocked());
    QTRY_VERIFY_WITH_TIMEOUT(!env.page->isBusy(), 5000);
    QVERIFY(!env.platform->gate()->isLocked());
    QVERIFY(!QFileInfo::exists(matchedPath));
    QVERIFY(!QFileInfo::exists(nestedDir + QStringLiteral("/my_report_2024.md")));
    QVERIFY(QFileInfo::exists(unmatchedPath));
    QVERIFY(QFileInfo(nestedDir).isDir());
    QVERIFY(QFileInfo(root).isDir());
    QCOMPARE(env.page->findChild<QLabel *>(QStringLiteral("filterUnique.matchedCount"))->text(),
             QStringLiteral("0"));
    QCOMPARE(env.page->findChild<QLabel *>(QStringLiteral("filterUnique.unmatchedCount"))->text(),
             QStringLiteral("1"));
}

void FilterUniqueTests::deleteFailureIsListed()
{
    Env env;
    const QString opened = openTool(&env);
    QVERIFY2(opened.isEmpty(), qPrintable(opened));
    const QString root = env.temp.filePath(QStringLiteral("scan"));
    const QString blocked = root + QStringLiteral("/a.txt");
    const QString gone = root + QStringLiteral("/b.txt");
    QVERIFY(writeFile(blocked).isEmpty());
    QVERIFY(writeFile(gone).isEmpty());

    auto *keywords = env.page->findChild<QPlainTextEdit *>(QStringLiteral("filterUnique.keywords"));
    auto *directory = env.page->findChild<QLineEdit *>(QStringLiteral("filterUnique.directory"));
    auto *start = env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.start"));
    keywords->setPlainText(QStringLiteral("txt"));
    directory->setText(root);
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(!env.page->isBusy(), 5000);

    QVERIFY(QFile::remove(blocked));
    QVERIFY(QDir().mkpath(blocked));

    env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.deleteMatched"))->click();
    env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.previewContinue"))->click();
    env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.confirmDelete"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(!env.page->isBusy(), 5000);

    QVERIFY(QFileInfo(blocked).isDir());
    QVERIFY(!QFileInfo::exists(gone));
    QVERIFY(QFileInfo(root).isDir());
    auto *problems = env.page->findChild<QTableView *>(QStringLiteral("filterUnique.problems"));
    QVERIFY(problems);
    QVERIFY(problems->model()->rowCount() >= 1);
    bool sawReason = false;
    for (int row = 0; row < problems->model()->rowCount(); ++row) {
        const QString reason = problems->model()->index(row, 1).data().toString();
        if (reason.contains(QStringLiteral("拒绝删除目录")))
            sawReason = true;
    }
    QVERIFY(sawReason);
    QCOMPARE(env.platform->status()->current(QStringLiteral("filter-unique")).state, StatusState::Error);
    QVERIFY(env.page->findChild<QLabel *>(QStringLiteral("filterUnique.detail"))
                ->text()
                .contains(QStringLiteral("失败")));
}

void FilterUniqueTests::cancelScanAndDeleteLeaveFilesUntouched()
{
    Env env;
    const QString opened = openTool(&env);
    QVERIFY2(opened.isEmpty(), qPrintable(opened));
    const QString root = env.temp.filePath(QStringLiteral("scan"));
    QStringList files;
    for (int i = 0; i < 8; ++i) {
        const QString path = root + QStringLiteral("/f%1.txt").arg(i);
        QVERIFY(writeFile(path).isEmpty());
        files.append(path);
    }

    auto *keywords = env.page->findChild<QPlainTextEdit *>(QStringLiteral("filterUnique.keywords"));
    auto *directory = env.page->findChild<QLineEdit *>(QStringLiteral("filterUnique.directory"));
    auto *start = env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.start"));
    auto *cancel = env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.cancel"));
    keywords->setPlainText(QStringLiteral("txt"));
    directory->setText(root);
    env.page->setStepDelayMs(400);
    start->click();
    QVERIFY(env.page->isBusy());
    QVERIFY(env.platform->gate()->isLocked());
    QVERIFY(cancel->isEnabled());
    cancel->click();
    QTRY_VERIFY_WITH_TIMEOUT(!env.page->isBusy(), 5000);
    QVERIFY(!env.platform->gate()->isLocked());
    QCOMPARE(env.page->findChild<QLabel *>(QStringLiteral("filterUnique.totalCount"))->text(),
             QStringLiteral("0"));
    QVERIFY(env.platform->status()->current(QStringLiteral("filter-unique")).text.contains(QStringLiteral("已取消筛选")));
    for (const QString &path : files)
        QVERIFY(QFileInfo::exists(path));

    env.page->setStepDelayMs(0);
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(!env.page->isBusy(), 5000);
    QCOMPARE(env.page->findChild<QLabel *>(QStringLiteral("filterUnique.matchedCount"))->text(),
             QStringLiteral("8"));

    env.page->setStepDelayMs(400);
    env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.deleteMatched"))->click();
    env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.previewContinue"))->click();
    env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.confirmDelete"))->click();
    QVERIFY(env.page->isBusy());
    QVERIFY(env.platform->gate()->isLocked());
    cancel->click();
    QTRY_VERIFY_WITH_TIMEOUT(!env.page->isBusy(), 5000);
    QVERIFY(!env.platform->gate()->isLocked());
    QVERIFY(env.platform->status()->current(QStringLiteral("filter-unique")).text.contains(QStringLiteral("已取消删除")));
    for (const QString &path : files)
        QVERIFY(QFileInfo::exists(path));
    QVERIFY(QFileInfo(root).isDir());
}

void FilterUniqueTests::sessionStaysInsideDataDir()
{
    Env env;
    const QString opened = openTool(&env);
    QVERIFY2(opened.isEmpty(), qPrintable(opened));
    const QString keywords = QStringLiteral("名单甲\n名单乙");
    const QString directory = env.temp.filePath(QStringLiteral("scan"));
    QVERIFY(QDir().mkpath(directory));
    env.page->findChild<QPlainTextEdit *>(QStringLiteral("filterUnique.keywords"))->setPlainText(keywords);
    env.page->findChild<QLineEdit *>(QStringLiteral("filterUnique.directory"))->setText(directory);
    env.page->findChild<QPushButton *>(QStringLiteral("filterUnique.start"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(!env.page->isBusy(), 5000);

    const QString dataDir = env.platform->paths().toolDataDir(QStringLiteral("filter-unique"));
    const QString sessionPath = QDir(dataDir).filePath(QStringLiteral("session.json"));
    QVERIFY(sessionPath.endsWith(QStringLiteral("/tools/filter-unique/session.json")));
    QCOMPARE(QFileInfo(sessionPath).dir().dirName(), QStringLiteral("filter-unique"));
    QFile sessionFile(sessionPath);
    QVERIFY(sessionFile.open(QIODevice::ReadOnly));
    const QJsonObject session = QJsonDocument::fromJson(sessionFile.readAll()).object();
    QCOMPARE(session.value(QStringLiteral("keywords")).toString(), keywords);
    QCOMPARE(session.value(QStringLiteral("directory")).toString(), directory);

    env.tool->shutdown();
    FilterUniqueTool reloaded;
    ToolContext context;
    context.info = reloaded.info();
    context.services = env.platform->servicesFor(context.info.id);
    QString error;
    QVERIFY(reloaded.initialize(context, &error));
    const QVector<ToolPage> pages = reloaded.createPages();
    QCOMPARE(pages.size(), 1);
    auto *page = qobject_cast<FilterUniquePage *>(pages.at(0).widget);
    QVERIFY(page);
    QCOMPARE(page->findChild<QPlainTextEdit *>(QStringLiteral("filterUnique.keywords"))->toPlainText(), keywords);
    QCOMPARE(page->findChild<QLineEdit *>(QStringLiteral("filterUnique.directory"))->text(), directory);
    delete page;

    if (geteuid() != 0) {
        const QString hidden = directory + QStringLiteral("/secret");
        QVERIFY(writeFile(hidden + QStringLiteral("/hidden.txt")).isEmpty());
        QVERIFY(QFile::setPermissions(hidden, QFileDevice::ExeOwner));
        std::atomic<bool> cancel{false};
        const FilterScanResult result = scanDirectory(directory, {QStringLiteral("hidden")}, cancel, {});
        bool unreadable = false;
        for (const FilterProblem &problem : result.problems) {
            if (problem.path == hidden && problem.reason.contains(QStringLiteral("无法读取目录")))
                unreadable = true;
        }
        QVERIFY(unreadable);
        QFile::setPermissions(hidden,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
}

QTEST_MAIN(FilterUniqueTests)
#include "filter_unique_test.moc"
