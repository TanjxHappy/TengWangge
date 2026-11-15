#include "tools/core_analyze/core_analyze_tool.h"

#include "core/data_paths.h"
#include "core/platform_services.h"
#include "shell/theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <memory>

namespace {

class ScriptedExecutor : public tw::CommandExecutor
{
public:
    QHash<QString, tw::CommandResult> byBase;
    QVector<tw::CommandRequest> calls;
    int delayMs = 0;
    std::atomic<bool> *cancel = nullptr;

    tw::CommandResult run(const tw::CommandRequest &request) override
    {
        calls.append(request);
        if (delayMs > 0) {
            const int slices = qMax(1, delayMs / 20);
            for (int i = 0; i < slices; ++i) {
                if (cancel && cancel->load()) {
                    tw::CommandResult cancelled;
                    cancelled.status = tw::CommandStatus::Cancelled;
                    cancelled.errorDetail = QStringLiteral("已取消");
                    return cancelled;
                }
                QThread::msleep(20);
            }
        }
        const QString base = QFileInfo(request.program).fileName();
        if (byBase.contains(base))
            return byBase.value(base);
        tw::CommandResult missing;
        missing.status = tw::CommandStatus::MissingProgram;
        missing.errorDetail = QStringLiteral("scripted miss");
        return missing;
    }
};

tw::CommandResult ran(const QString &output)
{
    tw::CommandResult result;
    result.status = tw::CommandStatus::Ran;
    result.exitCode = 0;
    result.output = output;
    return result;
}

tw::ToolFinder presentTools()
{
    return [](const QString &name) {
        const QStringList known = {QStringLiteral("file"), QStringLiteral("eu-readelf"),
                                   QStringLiteral("readelf"), QStringLiteral("eu-unstrip"),
                                   QStringLiteral("gdb")};
        if (known.contains(name))
            return QStringLiteral("/usr/bin/") + name;
        return QString();
    };
}

QByteArray elfHeader(int type)
{
    QByteArray header(64, '\0');
    header[0] = 0x7f;
    header[1] = 'E';
    header[2] = 'L';
    header[3] = 'F';
    header[4] = 2;
    header[5] = 1;
    header[6] = 1;
    header[16] = static_cast<char>(type);
    header[18] = 0x3e;
    header[20] = 1;
    return header;
}

QString writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    file.write(bytes);
    return QFileInfo(path).absoluteFilePath();
}

void appendU32(QByteArray *out, quint32 value)
{
    out->append(char(value & 0xff));
    out->append(char((value >> 8) & 0xff));
    out->append(char((value >> 16) & 0xff));
    out->append(char((value >> 24) & 0xff));
}

QByteArray coreWithSiginfo(quint32 signo, quint64 address)
{
    QByteArray desc(128, '\0');
    auto put32 = [&](int offset, quint32 value) {
        desc[offset] = char(value & 0xff);
        desc[offset + 1] = char((value >> 8) & 0xff);
        desc[offset + 2] = char((value >> 16) & 0xff);
        desc[offset + 3] = char((value >> 24) & 0xff);
    };
    put32(0, signo);
    put32(8, 1);
    for (int i = 0; i < 8; ++i)
        desc[16 + i] = char((address >> (8 * i)) & 0xff);

    QByteArray note;
    appendU32(&note, 5);
    appendU32(&note, quint32(desc.size()));
    appendU32(&note, 0x53494749);
    note.append("CORE", 4);
    note.append('\0');
    while (note.size() % 4 != 0)
        note.append('\0');
    note.append(desc);
    while (note.size() % 4 != 0)
        note.append('\0');

    const int phoff = 64;
    const int noteOff = phoff + 56;
    QByteArray file(noteOff + note.size(), '\0');
    file[0] = 0x7f;
    file[1] = 'E';
    file[2] = 'L';
    file[3] = 'F';
    file[4] = 2;
    file[5] = 1;
    file[6] = 1;
    file[16] = 4;
    file[18] = 0x3e;
    file[20] = 1;
    file[32] = char(phoff);
    file[52] = 64;
    file[54] = 56;
    file[56] = 1;
    file[64] = 4;
    file[72] = char(noteOff);
    const quint64 filesz = quint64(note.size());
    for (int i = 0; i < 8; ++i)
        file[96 + i] = char((filesz >> (8 * i)) & 0xff);
    file[112] = 4;
    for (int i = 0; i < note.size(); ++i)
        file[noteOff + i] = note.at(i);
    return file;
}

bool widgetInside(QWidget *root, QWidget *child)
{
    if (!root || !child || !child->isVisible())
        return false;
    const QPoint origin = child->mapTo(root, QPoint(0, 0));
    return root->rect().contains(QRect(origin, child->size()));
}

const QString kFileOut = QStringLiteral(
    "ELF 64-bit LSB core file, x86-64, version 1 (SYSV), from 'demo', execfn: '/tmp/demo'");
const QString kReadelfOut = QStringLiteral(
    "info.si_signo: 11, info.si_code: 1, info.si_errno: 0, cursig: 11\n"
    "si_addr: 0x0\n"
    "fname: demo\n"
    "psargs: /tmp/demo\n");
const QString kGdbOut = QStringLiteral(
    "Core was generated by `/tmp/demo'.\n"
    "Program terminated with signal SIGSEGV, Segmentation fault.\n"
    "#0  0x0000000000401131 in main () at demo.c:4\n"
    "#1  0x00007ffff7dca1ca in __libc_start_call_main () from /lib/libc.so.6\n"
    "\n"
    "Thread 1 (LWP 1):\n"
    "#0  0x0000000000401131 in main () at demo.c:4\n"
    "#1  0x00007ffff7dca1ca in __libc_start_call_main () from /lib/libc.so.6\n");

void shot(QWidget *widget, const QString &name)
{
    const QString dir = qEnvironmentVariable("TENGWANGGE_SCREENSHOT_DIR");
    if (dir.isEmpty() || !widget)
        return;
    QApplication::processEvents();
    widget->grab().save(dir + QLatin1Char('/') + name + QStringLiteral(".png"), "PNG");
}

void fillCrashOutputs(ScriptedExecutor *exec)
{
    exec->byBase.insert(QStringLiteral("file"), ran(kFileOut));
    exec->byBase.insert(QStringLiteral("eu-readelf"), ran(kReadelfOut));
    exec->byBase.insert(QStringLiteral("eu-unstrip"), ran(QStringLiteral("0x400000+0x1000 . . demo")));
    exec->byBase.insert(QStringLiteral("gdb"), ran(kGdbOut));
}

class PageHarness
{
public:
    explicit PageHarness(const QString &root)
        : platform(tw::DataPaths(root))
    {
        tw::ToolContext context;
        context.info = tool.info();
        context.services = platform.servicesFor(tool.info().id);
        QString error;
        initialized = tool.initialize(context, &error);
        initError = error;
        if (!initialized)
            return;
        pages = tool.createPages();
        page = tool.page();
    }

    ~PageHarness()
    {
        tool.shutdown();
        for (const tw::ToolPage &item : pages)
            delete item.widget;
    }

    tw::PlatformServices platform;
    tw::CoreAnalyzeTool tool;
    bool initialized = false;
    QString initError;
    QVector<tw::ToolPage> pages;
    tw::CoreAnalyzePage *page = nullptr;
};

} // namespace

class CoreAnalyzeTests : public QObject
{
    Q_OBJECT

private slots:
    void usageGuideListsCommands();
    void invalidFileReportsErrorAndSkipsDebugger();
    void programFileIsNotACore();
    void missingFileReportsError();
    void reportExplainsSignalCauseAndCommands();
    void commandsAreArgvNotAShell();
    void missingGdbStillExplainsSignal();
    void initializeCreatesNoWidgets();
    void pageRejectsInvalidFile();
    void pageHoldsNavigationLockUntilFinished();
    void pageCancelReleasesLock();
    void pageShowsScriptedReport();
    void saveReportWritesUtf8();
    void realCoreSampleWhenEnvironmentAllows();
    void notesSupplySignalWhenReadelfPrintsNoNumber();
    void readelfNumberWinsOverEmbeddedNotes();
    void faultAddressWordingFromEuReadelf();
    void currentThreadBeatsLongerStack();
    void sigquitUsesPlainLanguage();
    void unreadableFileIsNotCalledNotACore();
    void mismatchedExecutableSoftensWhere();
    void pageKeepsConclusionReportAndExportVisible();
};

void CoreAnalyzeTests::usageGuideListsCommands()
{
    const QString guide = tw::CoreAnalyzer::usageGuide();
    QVERIFY(guide.contains(QStringLiteral("如何查看和分析 core 文件")));
    QVERIFY(guide.contains(QStringLiteral("file -b --")));
    QVERIFY(guide.contains(QStringLiteral("eu-readelf")));
    QVERIFY(guide.contains(QStringLiteral("eu-unstrip")));
    QVERIFY(guide.contains(QStringLiteral("gdb -batch")));
    QVERIFY(guide.contains(QStringLiteral("释义")));
}

void CoreAnalyzeTests::invalidFileReportsErrorAndSkipsDebugger()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("notes.txt")), QByteArray("hello\n"));
    ScriptedExecutor exec;
    exec.byBase.insert(QStringLiteral("file"), ran(QStringLiteral("ASCII text")));
    tw::AnalysisRequest request;
    request.corePath = path;
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, presentTools());
    QVERIFY(!report.ok);
    QVERIFY(!report.isCore);
    QVERIFY(report.errorInfo.contains(QStringLiteral("不是 Linux core 文件")));
    QVERIFY(report.rendered.contains(QStringLiteral("释义")));
    QVERIFY(report.rendered.contains(QStringLiteral("ASCII text")));
    QVERIFY(report.rendered.contains(QStringLiteral("file")));
    for (const tw::CommandRequest &call : exec.calls) {
        QVERIFY(QFileInfo(call.program).fileName() != QStringLiteral("gdb"));
        QVERIFY(QFileInfo(call.program).fileName() != QStringLiteral("sh"));
    }
}

void CoreAnalyzeTests::programFileIsNotACore()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("app")), elfHeader(2));
    ScriptedExecutor exec;
    exec.byBase.insert(QStringLiteral("file"), ran(QStringLiteral("ELF 64-bit LSB pie executable, x86-64")));
    tw::AnalysisRequest request;
    request.corePath = path;
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, presentTools());
    QVERIFY(!report.isCore);
    QVERIFY(report.errorInfo.contains(QStringLiteral("不是 Linux core 文件")));
    QVERIFY(report.errorInfo.contains(QStringLiteral("程序文件")));
}

void CoreAnalyzeTests::missingFileReportsError()
{
    ScriptedExecutor exec;
    tw::AnalysisRequest request;
    request.corePath = QStringLiteral("/tmp/tengwangge-missing-core-d9fb");
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, presentTools());
    QVERIFY(!report.ok);
    QVERIFY(report.errorInfo.contains(QStringLiteral("找不到")));
    QVERIFY(exec.calls.isEmpty());
    QVERIFY(report.rendered.contains(QStringLiteral("file")));
    QVERIFY(report.rendered.contains(QStringLiteral("未执行")));
}

void CoreAnalyzeTests::reportExplainsSignalCauseAndCommands()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("demo.core")), elfHeader(4));
    ScriptedExecutor exec;
    fillCrashOutputs(&exec);
    tw::AnalysisRequest request;
    request.corePath = path;
    request.executablePath = QStringLiteral("/bin/true");
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, presentTools());
    QVERIFY(report.ok);
    QVERIFY(report.isCore);
    QCOMPARE(report.signalNumber, 11);
    QCOMPARE(report.signalName, QStringLiteral("SIGSEGV"));
    QVERIFY(report.errorInfo.contains(QStringLiteral("不允许使用的内存")));
    QVERIFY(report.errorInfo.contains(QStringLiteral("SIGSEGV")));
    QVERIFY(report.whereInfo.contains(QStringLiteral("main")));
    QVERIFY(report.whereInfo.contains(QStringLiteral("demo.c:4")));
    QVERIFY(report.cause.contains(QStringLiteral("空")));
    QVERIFY(report.suggestionText.contains(QStringLiteral("不要删除")));
    QVERIFY(report.rendered.contains(QStringLiteral("如何查看和分析 core 文件")));
    QVERIFY(report.rendered.contains(QStringLiteral("释义")));
    QVERIFY(report.rendered.contains(QStringLiteral("建议解决方案")));
    QVERIFY(report.rendered.contains(QStringLiteral("-b")));
    QVERIFY(report.rendered.contains(QStringLiteral("--core")));
    QVERIFY(report.rendered.contains(QStringLiteral("thread apply all bt")));
    QVERIFY(report.rendered.contains(path));
    QCOMPARE(QString::fromUtf8(report.rendered.toUtf8()), report.rendered);

    const tw::CommandRequest *gdb = nullptr;
    for (const tw::CommandRequest &call : exec.calls) {
        if (QFileInfo(call.program).fileName() == QStringLiteral("gdb"))
            gdb = &call;
    }
    QVERIFY(gdb);
    QVERIFY(gdb->arguments.contains(path));
    QVERIFY(gdb->arguments.contains(QStringLiteral("/bin/true")));
    QVERIFY(!gdb->arguments.contains(QStringLiteral("-c")));
}

void CoreAnalyzeTests::commandsAreArgvNotAShell()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString corePath = writeFile(dir.filePath(QStringLiteral("core;id")), elfHeader(4));
    const QString symbolDir = dir.filePath(QStringLiteral("sym dir"));
    QVERIFY(QDir().mkpath(symbolDir));
    ScriptedExecutor exec;
    fillCrashOutputs(&exec);
    tw::AnalysisRequest request;
    request.corePath = corePath;
    request.symbolPath = symbolDir;
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, presentTools());
    QVERIFY(report.ok);
    const tw::CommandRequest *gdb = nullptr;
    for (const tw::CommandRequest &call : exec.calls) {
        const QString base = QFileInfo(call.program).fileName();
        QVERIFY(base != QStringLiteral("sh"));
        QVERIFY(base != QStringLiteral("bash"));
        QVERIFY(base != QStringLiteral("dash"));
        QVERIFY(!call.arguments.contains(QStringLiteral("sh")));
        if (base == QStringLiteral("gdb"))
            gdb = &call;
    }
    QVERIFY(gdb);
    const int coreFlag = gdb->arguments.indexOf(QStringLiteral("-c"));
    QVERIFY(coreFlag >= 0);
    QCOMPARE(gdb->arguments.at(coreFlag + 1), QFileInfo(corePath).absoluteFilePath());
    int directoryArgs = 0;
    for (const QString &argument : gdb->arguments) {
        if (argument.contains(QStringLiteral("debug-file-directory"))) {
            ++directoryArgs;
            QVERIFY(argument.contains(QFileInfo(symbolDir).absoluteFilePath()));
            QVERIFY(argument.contains(QStringLiteral("/usr/lib/debug")));
            QVERIFY(!argument.contains(QLatin1Char('"')));
        }
    }
    QCOMPARE(directoryArgs, 1);
    QVERIFY(report.rendered.contains(QLatin1Char('\'')));
    QVERIFY(gdb->arguments.contains(QStringLiteral("bt")));
    QVERIFY(gdb->arguments.contains(QStringLiteral("thread")));
}

void CoreAnalyzeTests::missingGdbStillExplainsSignal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("demo.core")), elfHeader(4));
    ScriptedExecutor exec;
    exec.byBase.insert(QStringLiteral("file"), ran(kFileOut));
    exec.byBase.insert(QStringLiteral("eu-readelf"), ran(kReadelfOut));
    auto finder = [](const QString &name) {
        if (name == QLatin1String("file") || name == QLatin1String("eu-readelf"))
            return QStringLiteral("/usr/bin/") + name;
        return QString();
    };
    tw::AnalysisRequest request;
    request.corePath = path;
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, finder);
    QVERIFY(report.ok);
    QCOMPARE(report.signalNumber, 11);
    QVERIFY(report.errorInfo.contains(QStringLiteral("不允许使用的内存")));
    QVERIFY(report.rendered.contains(QStringLiteral("没有找到 gdb")));
    QVERIFY(report.suggestionText.contains(QStringLiteral("安装 gdb")));
    for (const tw::CommandRequest &call : exec.calls)
        QVERIFY(QFileInfo(call.program).fileName() != QStringLiteral("gdb"));
}

void CoreAnalyzeTests::initializeCreatesNoWidgets()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const int before = QApplication::allWidgets().size();
    tw::PlatformServices platform(tw::DataPaths(dir.filePath(QStringLiteral("data"))));
    tw::CoreAnalyzeTool tool;
    tw::ToolContext context;
    context.info = tool.info();
    context.services = platform.servicesFor(tool.info().id);
    QString error;
    QVERIFY(tool.initialize(context, &error));
    QCOMPARE(QApplication::allWidgets().size(), before);
    QCOMPARE(tool.info().id, QStringLiteral("core-analyze"));
    QCOMPARE(tool.info().title, QStringLiteral("文件分析"));
}

void CoreAnalyzeTests::pageRejectsInvalidFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("notes.txt")), QByteArray("not a core\n"));
    PageHarness harness(dir.filePath(QStringLiteral("data")));
    QVERIFY2(harness.initialized, qPrintable(harness.initError));
    QVERIFY(harness.page);
    harness.page->resize(960, 720);
    harness.page->show();
    harness.page->findChild<QLineEdit *>(QStringLiteral("corePath"))->setText(path);
    harness.page->findChild<QPushButton *>(QStringLiteral("startButton"))->click();
    auto *error = harness.page->findChild<QLabel *>(QStringLiteral("errorValue"));
    QTRY_VERIFY_WITH_TIMEOUT(error->text().contains(QStringLiteral("不是 Linux core 文件")), 10000);
    auto *report = harness.page->findChild<QPlainTextEdit *>(QStringLiteral("reportView"));
    QVERIFY(report->toPlainText().contains(QStringLiteral("释义")));
    QVERIFY(report->toPlainText().contains(QStringLiteral("file")));
    QVERIFY(!harness.platform.gate()->isLocked());
    shot(harness.page, QStringLiteral("core-analyze-invalid"));
}

void CoreAnalyzeTests::pageHoldsNavigationLockUntilFinished()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("notes.txt")), QByteArray("not a core\n"));
    PageHarness harness(dir.filePath(QStringLiteral("data")));
    QVERIFY(harness.initialized);
    harness.page->setFinderForTest(presentTools());
    harness.page->setExecutorFactoryForTest([](std::atomic<bool> *cancel) {
        auto exec = std::make_unique<ScriptedExecutor>();
        exec->cancel = cancel;
        exec->delayMs = 800;
        exec->byBase.insert(QStringLiteral("file"), ran(QStringLiteral("ASCII text")));
        return exec;
    });
    harness.page->show();
    harness.page->findChild<QLineEdit *>(QStringLiteral("corePath"))->setText(path);
    harness.page->findChild<QPushButton *>(QStringLiteral("startButton"))->click();
    QVERIFY(harness.platform.gate()->isLocked());
    QVERIFY(!harness.page->findChild<QPushButton *>(QStringLiteral("startButton"))->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(!harness.platform.gate()->isLocked(), 5000);
    QVERIFY(harness.page->findChild<QLabel *>(QStringLiteral("errorValue"))->text().contains(
        QStringLiteral("不是 Linux core 文件")));
}

void CoreAnalyzeTests::pageCancelReleasesLock()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("notes.txt")), QByteArray("not a core\n"));
    PageHarness harness(dir.filePath(QStringLiteral("data")));
    QVERIFY(harness.initialized);
    harness.page->setFinderForTest(presentTools());
    harness.page->setExecutorFactoryForTest([](std::atomic<bool> *cancel) {
        auto exec = std::make_unique<ScriptedExecutor>();
        exec->cancel = cancel;
        exec->delayMs = 4000;
        exec->byBase.insert(QStringLiteral("file"), ran(QStringLiteral("ASCII text")));
        return exec;
    });
    harness.page->show();
    harness.page->findChild<QLineEdit *>(QStringLiteral("corePath"))->setText(path);
    harness.page->findChild<QPushButton *>(QStringLiteral("startButton"))->click();
    QVERIFY(harness.platform.gate()->isLocked());
    harness.page->findChild<QPushButton *>(QStringLiteral("cancelButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.platform.gate()->isLocked(), 5000);
    QVERIFY(harness.page->findChild<QLabel *>(QStringLiteral("errorValue"))->text().contains(QStringLiteral("取消")));
    QCOMPARE(harness.platform.status()->current(QStringLiteral("core-analyze")).state, tw::StatusState::Info);
}

void CoreAnalyzeTests::pageShowsScriptedReport()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("demo.core")), elfHeader(4));
    PageHarness harness(dir.filePath(QStringLiteral("data")));
    QVERIFY(harness.initialized);
    harness.page->setFinderForTest(presentTools());
    harness.page->setExecutorFactoryForTest([](std::atomic<bool> *cancel) {
        auto exec = std::make_unique<ScriptedExecutor>();
        exec->cancel = cancel;
        fillCrashOutputs(exec.get());
        return exec;
    });
    harness.page->resize(1100, 860);
    harness.page->show();
    harness.page->findChild<QLineEdit *>(QStringLiteral("corePath"))->setText(path);
    harness.page->findChild<QPushButton *>(QStringLiteral("startButton"))->click();
    auto *error = harness.page->findChild<QLabel *>(QStringLiteral("errorValue"));
    QTRY_VERIFY_WITH_TIMEOUT(error->text().contains(QStringLiteral("不允许使用的内存")), 5000);
    if (auto *scroll = harness.page->findChild<QScrollArea *>())
        scroll->ensureWidgetVisible(error);
    shot(harness.page, QStringLiteral("core-analyze-result"));
    const QString report = harness.page->findChild<QPlainTextEdit *>(QStringLiteral("reportView"))->toPlainText();
    QVERIFY(report.contains(QStringLiteral("释义")));
    QVERIFY(report.contains(QStringLiteral("thread apply all bt")));
    QVERIFY(report.contains(QStringLiteral("建议解决方案")));
    auto *suggestion = harness.page->findChild<QLabel *>(QStringLiteral("suggestionValue"));
    auto *conclusion = harness.page->findChild<QScrollArea *>(QStringLiteral("conclusionScroll"));
    QVERIFY(suggestion);
    QVERIFY(conclusion);
    const QPoint suggestionTop = suggestion->mapTo(conclusion->viewport(), QPoint(0, 0));
    const QRect suggestionRect(suggestionTop, suggestion->size());
    QVERIFY2(conclusion->viewport()->rect().contains(suggestionRect),
             "典型结论的建议被结论区裁掉了");
    QCOMPARE(harness.platform.status()->current(QStringLiteral("core-analyze")).state, tw::StatusState::Success);
    QVERIFY(!harness.platform.gate()->isLocked());
}

void CoreAnalyzeTests::saveReportWritesUtf8()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PageHarness harness(dir.filePath(QStringLiteral("data")));
    QVERIFY(harness.initialized);
    const QString path = dir.filePath(QStringLiteral("说明.txt"));
    QString error;
    QVERIFY2(harness.page->saveReport(path, &error), qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    QVERIFY(!bytes.startsWith(QByteArray("\xEF\xBB\xBF", 3)));
    const QString text = QString::fromUtf8(bytes);
    QVERIFY(text.contains(QStringLiteral("如何查看和分析 core 文件")));
    QVERIFY(text.contains(QStringLiteral("释义")));
    QCOMPARE(text, harness.page->findChild<QPlainTextEdit *>(QStringLiteral("reportView"))->toPlainText());
}

void CoreAnalyzeTests::realCoreSampleWhenEnvironmentAllows()
{
    if (QStandardPaths::findExecutable(QStringLiteral("gcc")).isEmpty())
        QSKIP("gcc 不在 PATH 中");
    QFile patternFile(QStringLiteral("/proc/sys/kernel/core_pattern"));
    if (!patternFile.open(QIODevice::ReadOnly))
        QSKIP("读不到 core_pattern");
    const QString pattern = QString::fromLocal8Bit(patternFile.readAll()).trimmed();
    if (pattern.startsWith(QLatin1Char('|')))
        QSKIP("core 交给了外部程序，无法在测试里拿到文件");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("crash.c"));
    writeFile(source, QByteArray(
        "#include <sys/resource.h>\n"
        "int main(void) {\n"
        "  struct rlimit lim;\n"
        "  lim.rlim_cur = RLIM_INFINITY;\n"
        "  lim.rlim_max = RLIM_INFINITY;\n"
        "  setrlimit(RLIMIT_CORE, &lim);\n"
        "  volatile int *p = 0;\n"
        "  *p = 1;\n"
        "  return 0;\n"
        "}\n"));
    const QString binary = dir.filePath(QStringLiteral("twcore"));
    QProcess gcc;
    gcc.start(QStringLiteral("gcc"),
              {QStringLiteral("-g"), QStringLiteral("-O0"), QStringLiteral("-fno-omit-frame-pointer"),
               QStringLiteral("-o"), binary, source});
    QVERIFY(gcc.waitForFinished(20000));
    if (gcc.exitCode() != 0)
        QSKIP("gcc 没有编译出样例");

    QProcess crash;
    crash.setProgram(binary);
    crash.setWorkingDirectory(dir.path());
    crash.start();
    if (!crash.waitForStarted(3000))
        QSKIP("样例进程没有启动");
    const qint64 pid = crash.processId();
    crash.waitForFinished(5000);

    bool usesPid = false;
    QFile usesPidFile(QStringLiteral("/proc/sys/kernel/core_uses_pid"));
    if (usesPidFile.open(QIODevice::ReadOnly))
        usesPid = usesPidFile.readAll().trimmed() == "1";

    QString corePath;
    bool supported = true;
    const QString exeName = QStringLiteral("twcore");
    const QString exeBang = QFileInfo(binary).absoluteFilePath().replace(QLatin1Char('/'), QLatin1Char('!'));
    for (int i = 0; i < pattern.size() && supported; ++i) {
        if (pattern.at(i) != QLatin1Char('%')) {
            corePath += pattern.at(i);
            continue;
        }
        if (i + 1 >= pattern.size()) {
            supported = false;
            break;
        }
        const QChar code = pattern.at(++i);
        if (code == QLatin1Char('%'))
            corePath += QLatin1Char('%');
        else if (code == QLatin1Char('e'))
            corePath += exeName;
        else if (code == QLatin1Char('E'))
            corePath += exeBang;
        else if (code == QLatin1Char('p') || code == QLatin1Char('P') || code == QLatin1Char('i'))
            corePath += QString::number(pid);
        else
            supported = false;
    }
    if (supported && usesPid && !pattern.contains(QLatin1String("%p")) && !pattern.contains(QLatin1String("%P")))
        corePath += QLatin1Char('.') + QString::number(pid);
    if (!supported)
        QSKIP("认不出本机的 core 文件名规则");
    if (!QFileInfo::exists(corePath))
        QSKIP("没有生成 core 文件（可能被系统限制）");

    struct CoreCleanup {
        QString path;
        ~CoreCleanup() { QFile::remove(path); }
    } cleanup{corePath};

    std::atomic<bool> cancel{false};
    tw::ProcessExecutor executor(&cancel);
    tw::AnalysisRequest request;
    request.corePath = corePath;
    request.executablePath = binary;
    request.workDirectory = dir.path();
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, executor, {}, &cancel);
    QVERIFY(report.isCore);
    QVERIFY(report.rendered.contains(QStringLiteral("如何查看和分析 core 文件")));
    QVERIFY(report.rendered.contains(QStringLiteral("释义")));
    QVERIFY(report.rendered.contains(corePath));
    QVERIFY(report.signalNumber == 11 || report.rendered.contains(QStringLiteral("SIGSEGV")));
    QVERIFY(report.errorInfo.contains(QStringLiteral("不允许使用的内存"))
            || report.rendered.contains(QStringLiteral("SIGSEGV")));
    if (!QStandardPaths::findExecutable(QStringLiteral("gdb")).isEmpty())
        QVERIFY(report.whereInfo.contains(QStringLiteral("main")) || report.rendered.contains(QStringLiteral("main")));
}

void CoreAnalyzeTests::notesSupplySignalWhenReadelfPrintsNoNumber()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("demo.core")), coreWithSiginfo(11, 0));
    ScriptedExecutor exec;
    exec.byBase.insert(QStringLiteral("file"), ran(kFileOut));
    exec.byBase.insert(QStringLiteral("readelf"), ran(QStringLiteral(
        "CORE 0x150 NT_PRSTATUS (prstatus structure)\n"
        "CORE 0x80 NT_SIGINFO (siginfo_t data)\n"
        "Cannot decode 64-bit note in 32-bit build\n"
        "description data: 0b 00 00 00\n")));
    auto finder = [](const QString &name) {
        if (name == QLatin1String("file") || name == QLatin1String("readelf"))
            return QStringLiteral("/usr/bin/") + name;
        return QString();
    };
    tw::AnalysisRequest request;
    request.corePath = path;
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, finder);
    QVERIFY(report.ok);
    QCOMPARE(report.signalNumber, 11);
    QCOMPARE(report.signalName, QStringLiteral("SIGSEGV"));
    QVERIFY(report.errorInfo.contains(QStringLiteral("不允许使用的内存")));
    QVERIFY(report.cause.contains(QStringLiteral("空")));
    QVERIFY(report.rendered.contains(QStringLiteral("直接读取文件里的记录")));
    QVERIFY(report.rendered.contains(QStringLiteral("description data")));
    bool sawReadelf = false;
    for (const tw::CommandRequest &call : exec.calls) {
        const QString base = QFileInfo(call.program).fileName();
        QVERIFY(base != QStringLiteral("gdb"));
        QVERIFY(base != QStringLiteral("eu-readelf"));
        if (base == QStringLiteral("readelf"))
            sawReadelf = true;
    }
    QVERIFY(sawReadelf);
}

void CoreAnalyzeTests::readelfNumberWinsOverEmbeddedNotes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("demo.core")), coreWithSiginfo(11, 0));
    ScriptedExecutor exec;
    exec.byBase.insert(QStringLiteral("file"), ran(kFileOut));
    exec.byBase.insert(QStringLiteral("eu-readelf"), ran(QStringLiteral("cursig: 6\n")));
    tw::AnalysisRequest request;
    request.corePath = path;
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, presentTools());
    QCOMPARE(report.signalNumber, 6);
    QCOMPARE(report.signalName, QStringLiteral("SIGABRT"));
    QVERIFY(report.errorInfo.contains(QStringLiteral("自己停了下来")));
    QVERIFY(!report.cause.contains(QStringLiteral("空")));
    QVERIFY(!report.rendered.contains(QStringLiteral("直接读取文件里的记录")));
}

void CoreAnalyzeTests::faultAddressWordingFromEuReadelf()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("demo.core")), elfHeader(4));
    ScriptedExecutor exec;
    exec.byBase.insert(QStringLiteral("file"), ran(kFileOut));
    exec.byBase.insert(QStringLiteral("eu-readelf"), ran(QStringLiteral(
        "info.si_signo: 11, info.si_code: 1, info.si_errno: 0, cursig: 11\n"
        "fault address: 0\n")));
    tw::AnalysisRequest request;
    request.corePath = path;
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, presentTools());
    QCOMPARE(report.signalNumber, 11);
    QVERIFY(report.cause.contains(QStringLiteral("空")));
    QVERIFY(report.rendered.contains(QStringLiteral("fault address")));
}

void CoreAnalyzeTests::currentThreadBeatsLongerStack()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("demo.core")), elfHeader(4));
    ScriptedExecutor exec;
    fillCrashOutputs(&exec);
    exec.byBase.insert(QStringLiteral("gdb"), ran(QStringLiteral(
        "Program terminated with signal SIGSEGV, Segmentation fault.\n"
        "#0  0x401000 in worker () at worker.c:9\n"
        "#1  0x402000 in main () at main.c:3\n"
        "[Current thread is 2 (LWP 99)]\n"
        "Thread 1 (LWP 10):\n"
        "#0  0x1 in deep1 () at a.c:1\n"
        "#1  0x2 in deep2 () at a.c:2\n"
        "#2  0x3 in deep3 () at a.c:3\n"
        "#3  0x4 in deep4 () at a.c:4\n"
        "#4  0x5 in deep5 () at a.c:5\n"
        "#5  0x6 in deep6 () at a.c:6\n"
        "Thread 2 (LWP 99):\n"
        "#0  0x401000 in worker () at worker.c:9\n"
        "#1  0x402000 in main () at main.c:3\n")));
    tw::AnalysisRequest request;
    request.corePath = path;
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, presentTools());
    QVERIFY(report.whereInfo.contains(QStringLiteral("worker")));
    QVERIFY(report.whereInfo.contains(QStringLiteral("触发停止")));
    QVERIFY(!report.whereInfo.contains(QStringLiteral("deep6")));
}

void CoreAnalyzeTests::sigquitUsesPlainLanguage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("demo.core")), elfHeader(4));
    ScriptedExecutor exec;
    fillCrashOutputs(&exec);
    exec.byBase.insert(QStringLiteral("eu-readelf"), ran(QStringLiteral("cursig: 3\n")));
    exec.byBase.insert(QStringLiteral("gdb"), ran(QStringLiteral(
        "Program terminated with signal SIGQUIT, Quit.\n"
        "#0  0x401000 in main () at q.c:1\n")));
    tw::AnalysisRequest request;
    request.corePath = path;
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, presentTools());
    QCOMPARE(report.signalNumber, 3);
    QCOMPARE(report.signalName, QStringLiteral("SIGQUIT"));
    QVERIFY(report.errorInfo.contains(QStringLiteral("退出请求")));
    QVERIFY(report.suggestionText.contains(QStringLiteral("不要删除")));
}

void CoreAnalyzeTests::unreadableFileIsNotCalledNotACore()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("hidden.core")), coreWithSiginfo(11, 0));
    QVERIFY(QFile::setPermissions(path, QFileDevice::WriteOwner));
    struct RestorePermissions {
        QString path;
        ~RestorePermissions()
        {
            QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
    } restore{path};
    if (QFileInfo(path).isReadable())
        QSKIP("当前账户即使去掉读权限也能读取");

    ScriptedExecutor exec;
    exec.byBase.insert(QStringLiteral("file"), ran(QStringLiteral("ELF 64-bit LSB core file")));
    tw::AnalysisRequest request;
    request.corePath = path;
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, presentTools());
    QVERIFY(!report.ok);
    QVERIFY(report.errorInfo.contains(QStringLiteral("没有权限")));
    QVERIFY(!report.errorInfo.contains(QStringLiteral("不是 Linux core")));
    QVERIFY(exec.calls.isEmpty());
    QVERIFY(report.rendered.contains(QStringLiteral("未执行")));
}

void CoreAnalyzeTests::mismatchedExecutableSoftensWhere()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("demo.core")), elfHeader(4));
    ScriptedExecutor exec;
    fillCrashOutputs(&exec);
    exec.byBase.insert(QStringLiteral("gdb"), ran(QStringLiteral(
        "warning: core file may not match specified executable file.\n"
        "Program terminated with signal SIGSEGV, Segmentation fault.\n"
        "#0  0x401000 in imagined () at no.c:1\n")));
    tw::AnalysisRequest request;
    request.corePath = path;
    request.executablePath = QStringLiteral("/bin/true");
    const tw::AnalysisReport report = tw::CoreAnalyzer::analyze(request, exec, presentTools());
    QVERIFY(report.whereInfo.contains(QStringLiteral("对不上")));
    QVERIFY(report.whereInfo.contains(QStringLiteral("imagined")));
    QVERIFY(report.suggestionText.contains(QStringLiteral("再分析一次")));
}

void CoreAnalyzeTests::pageKeepsConclusionReportAndExportVisible()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("demo.core")), elfHeader(4));
    PageHarness harness(dir.filePath(QStringLiteral("data")));
    QVERIFY(harness.initialized);
    harness.page->setStyleSheet(tw::Theme::workbench().styleSheet());
    harness.page->resize(980, 640);
    harness.page->show();
    QApplication::processEvents();

    auto *exportButton = harness.page->findChild<QPushButton *>(QStringLiteral("exportButton"));
    auto *report = harness.page->findChild<QPlainTextEdit *>(QStringLiteral("reportView"));
    auto *conclusion = harness.page->findChild<QScrollArea *>(QStringLiteral("conclusionScroll"));
    QVERIFY(exportButton);
    QVERIFY(report);
    QVERIFY(conclusion);
    const auto topInPage = [&](QWidget *widget) {
        return widget->mapTo(harness.page, QPoint(0, 0)).y();
    };
    QVERIFY2(widgetInside(harness.page, exportButton), "导出按钮不在 1180x760 对应的页面里");
    QVERIFY2(widgetInside(harness.page, conclusion), "结论区不在页面里");
    QVERIFY2(widgetInside(harness.page, report), "分析说明不在页面里");
    QVERIFY(report->height() >= 100);
    QVERIFY(topInPage(exportButton) < topInPage(conclusion));
    QVERIFY(topInPage(conclusion) < topInPage(report));

    harness.page->setFinderForTest(presentTools());
    harness.page->setExecutorFactoryForTest([](std::atomic<bool> *cancel) {
        auto exec = std::make_unique<ScriptedExecutor>();
        exec->cancel = cancel;
        fillCrashOutputs(exec.get());
        return exec;
    });
    harness.page->findChild<QLineEdit *>(QStringLiteral("corePath"))->setText(path);
    harness.page->findChild<QPushButton *>(QStringLiteral("startButton"))->click();
    auto *error = harness.page->findChild<QLabel *>(QStringLiteral("errorValue"));
    QTRY_VERIFY_WITH_TIMEOUT(error->text().contains(QStringLiteral("不允许使用的内存")), 5000);
    harness.page->resize(980, 640);
    QApplication::processEvents();
    auto *suggestion = harness.page->findChild<QLabel *>(QStringLiteral("suggestionValue"));
    const QPoint suggestionTop = suggestion->mapTo(conclusion->viewport(), QPoint(0, 0));
    QVERIFY2(conclusion->viewport()->rect().contains(QRect(suggestionTop, suggestion->size())),
             "1180x760 下正常结论的建议没有完整显示");
    QVERIFY2(widgetInside(harness.page, exportButton), "正常结论把导出按钮挤出了页面");
    QVERIFY2(widgetInside(harness.page, report), "正常结论把分析说明挤出了页面");
    QVERIFY(report->height() >= 100);
    shot(harness.page, QStringLiteral("core-analyze-normal"));

    const QString longName(360, QLatin1Char('N'));
    harness.page->setExecutorFactoryForTest([longName](std::atomic<bool> *cancel) {
        auto exec = std::make_unique<ScriptedExecutor>();
        exec->cancel = cancel;
        fillCrashOutputs(exec.get());
        exec->byBase.insert(QStringLiteral("gdb"), ran(
            QStringLiteral("Program terminated with signal SIGSEGV, Segmentation fault.\n#0  0x401000 in ")
            + longName + QStringLiteral(" () at /tmp/very/long/path/crash.c:7\n")));
        exec->byBase.insert(QStringLiteral("file"), ran(
            QStringLiteral("ELF 64-bit LSB core file, from 'demo', execfn: '/tmp/")
            + longName + QStringLiteral("'\n")));
        return exec;
    });
    harness.page->findChild<QLineEdit *>(QStringLiteral("corePath"))->setText(path);
    harness.page->findChild<QPushButton *>(QStringLiteral("startButton"))->click();
    auto *where = harness.page->findChild<QLabel *>(QStringLiteral("whereValue"));
    QTRY_VERIFY_WITH_TIMEOUT(where->text().contains(longName.left(12)), 5000);
    QApplication::processEvents();
    harness.page->resize(980, 640);
    QApplication::processEvents();

    QVERIFY2(widgetInside(harness.page, exportButton), "长结论把导出按钮挤出了页面");
    QVERIFY2(widgetInside(harness.page, conclusion), "长结论把结论区挤出了页面");
    QVERIFY2(widgetInside(harness.page, report), "长结论把分析说明挤出了页面");
    QVERIFY(report->height() >= 100);
    QVERIFY(conclusion->height() <= 248);
    QVERIFY(exportButton->isEnabled());
    shot(harness.page, QStringLiteral("core-analyze-fit"));
}

QTEST_MAIN(CoreAnalyzeTests)
#include "core_analyze_tests.moc"
