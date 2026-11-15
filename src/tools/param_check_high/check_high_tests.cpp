#include "core/data_paths.h"
#include "core/platform_services.h"
#include "shell/main_window.h"
#include "shell/tool_registry.h"
#include "tools/param_check_high/byte_store.h"
#include "tools/param_check_high/check_high_page.h"
#include "tools/param_check_high/check_high_tool.h"
#include "tools/param_check_high/notation.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCodec>
#include <QTreeView>

#include <memory>

using namespace tw;

namespace {

QString readText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
}

bool directoryHasTemp(const QString &directory)
{
    const QStringList names = QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString &name : names) {
        if (name.contains(QLatin1String(".high-")))
            return true;
    }
    return false;
}

class ServiceFixture {
public:
    ServiceFixture()
        : platform(DataPaths(dir.path() + QStringLiteral("/root")))
        , services(platform.servicesFor(QStringLiteral("param-check-high")))
    {
    }

    QTemporaryDir dir;
    PlatformServices platform;
    ToolServices services;
};

} // namespace

class NotationTests : public QObject
{
    Q_OBJECT

private slots:
    void jsonKeepsOrderSpellingAndUnicode();
    void jsonExplainsBrokenText();
    void xmlKeepsDeclarationAttributesAndText();
    void xmlExplainsUnclosedTag();
    void iniKeepsCommentsSectionsAndQuotes();
    void iniSectionIsNotAJsonArray();
    void yamlIsRejected();
    void deepNestingStopsWithAPlainMessage();
    void hugeTextIsRefusedBeforeParsing();
    void atomicReplaceKeepsPermissionsAndNoTemp();
};

void NotationTests::jsonKeepsOrderSpellingAndUnicode()
{
    Sheet sheet = parseSheet(QStringLiteral("{\"z\":1.50,\"a\":\"\\u6ed5\\u738b\\u9601\",\"n\":[true,null]}"),
                             Notation::Json);
    QVERIFY2(sheet.ok, qPrintable(sheet.message));
    QCOMPARE(sheet.root->kind, NodeKind::Map);
    QCOMPARE(sheet.root->childCount(), 3);
    QCOMPARE(sheet.root->children[0]->name, QStringLiteral("z"));
    QCOMPARE(sheet.root->children[0]->spelling, QStringLiteral("1.50"));
    QCOMPARE(sheet.root->children[1]->value, QStringLiteral("滕王阁"));
    QCOMPARE(sheet.root->children[2]->children[0]->kind, NodeKind::Switch);
    QCOMPARE(sheet.root->children[2]->children[1]->kind, NodeKind::Vacant);

    const QString serialized = composeSheet(sheet);
    QVERIFY(serialized.indexOf(QStringLiteral("\"z\"")) >= 0);
    QVERIFY(serialized.indexOf(QStringLiteral("\"z\"")) < serialized.indexOf(QStringLiteral("\"a\"")));
    QVERIFY(serialized.contains(QStringLiteral("1.50")));
    QVERIFY(serialized.contains(QStringLiteral("滕王阁")));

    sheet.root->children[0]->value = QStringLiteral("2");
    sheet.root->children[0]->spelling = QStringLiteral("2");
    const Sheet again = parseSheet(composeSheet(sheet), Notation::Json);
    QVERIFY(again.ok);
    QCOMPARE(again.root->children[0]->spelling, QStringLiteral("2"));
}

void NotationTests::jsonExplainsBrokenText()
{
    const Sheet missing = parseSheet(QStringLiteral("{\"a\":"), Notation::Json);
    QVERIFY(!missing.ok);
    QVERIFY(missing.message.contains(QStringLiteral("行")));
    QVERIFY(missing.line >= 1);

    const Sheet duplicate = parseSheet(QStringLiteral("{\"a\":1,\"a\":2}"), Notation::Json);
    QVERIFY(!duplicate.ok);
    QVERIFY(duplicate.message.contains(QStringLiteral("重复的键")));

    const Sheet trailing = parseSheet(QStringLiteral("[1,]"), Notation::Json);
    QVERIFY(!trailing.ok);
    QVERIFY(trailing.message.contains(QStringLiteral("逗号")));
}

void NotationTests::xmlKeepsDeclarationAttributesAndText()
{
    const QString source = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<root id=\"1\"><name>甲</name></root>\n");
    Sheet sheet = parseSheet(source, Notation::Xml);
    QVERIFY2(sheet.ok, qPrintable(sheet.message));
    QVERIFY(sheet.prologue.contains(QStringLiteral("UTF-8")));
    Node *root = findNode(sheet.root.get(), {QStringLiteral("root")});
    QVERIFY(root != nullptr);
    QCOMPARE(displayedValue(findNode(root, {QStringLiteral("@id")})), QStringLiteral("1"));
    QCOMPARE(displayedValue(findNode(root, {QStringLiteral("name")})), QStringLiteral("甲"));

    Node *name = findNode(root, {QStringLiteral("name")});
    QVERIFY(name != nullptr);
    QVERIFY(name->childCount() >= 1);
    name->children[0]->value = QStringLiteral("乙");
    const QString serialized = composeSheet(sheet);
    QVERIFY(serialized.contains(QStringLiteral("UTF-8")));
    QVERIFY(serialized.contains(QStringLiteral("乙")));
    QVERIFY(serialized.contains(QStringLiteral("id=\"1\"")));
}

void NotationTests::xmlExplainsUnclosedTag()
{
    const Sheet sheet = parseSheet(QStringLiteral("<root>"), Notation::Xml);
    QVERIFY(!sheet.ok);
    QVERIFY(sheet.message.contains(QStringLiteral("XML")));
    QVERIFY(sheet.message.contains(QStringLiteral("行")));
    QVERIFY(sheet.line >= 1);
}

void NotationTests::iniKeepsCommentsSectionsAndQuotes()
{
    const QString source = QStringLiteral("# head\n[server]\nport=8080\nname=\"滕 王\"\n");
    Sheet sheet = parseSheet(source, Notation::Ini);
    QVERIFY2(sheet.ok, qPrintable(sheet.message));
    QCOMPARE(sheet.root->children[0]->kind, NodeKind::Comment);
    QCOMPARE(sheet.root->children[0]->spelling, QStringLiteral("#"));
    QCOMPARE(sheet.root->children[1]->name, QStringLiteral("server"));
    QCOMPARE(sheet.root->children[1]->children[0]->name, QStringLiteral("port"));
    QCOMPARE(sheet.root->children[1]->children[0]->value, QStringLiteral("8080"));
    QCOMPARE(sheet.root->children[1]->children[1]->value, QStringLiteral("滕 王"));

    sheet.root->children[1]->children[0]->value = QStringLiteral("9090");
    const QString serialized = composeSheet(sheet);
    QVERIFY(serialized.contains(QStringLiteral("[server]")));
    QVERIFY(serialized.contains(QStringLiteral("port=9090")));
    QVERIFY(serialized.contains(QStringLiteral("# head")));

    const Sheet broken = parseSheet(QStringLiteral("[server]\nnot a pair\n"), Notation::Ini);
    QVERIFY(!broken.ok);
    QVERIFY(broken.message.contains(QStringLiteral("第 2 行")));
}

void NotationTests::iniSectionIsNotAJsonArray()
{
    const Sheet ini = parseSheet(QStringLiteral("[server]\nport=8080\n"), Notation::Automatic);
    QVERIFY2(ini.ok, qPrintable(ini.message));
    QCOMPARE(static_cast<int>(ini.notation), static_cast<int>(Notation::Ini));

    const Sheet json = parseSheet(QStringLiteral("[1, 2]"), Notation::Automatic);
    QVERIFY2(json.ok, qPrintable(json.message));
    QCOMPARE(static_cast<int>(json.notation), static_cast<int>(Notation::Json));
    QCOMPARE(json.root->kind, NodeKind::List);
}

void NotationTests::yamlIsRejected()
{
    const Sheet sheet = parseSheet(QStringLiteral("name: value\n"), Notation::Automatic, QStringLiteral("yml"));
    QVERIFY(!sheet.ok);
    QVERIFY(sheet.message.contains(QStringLiteral("YAML")));
}

void NotationTests::deepNestingStopsWithAPlainMessage()
{
    QString deep;
    for (int i = 0; i < 80; ++i)
        deep += QStringLiteral("{\"d\":");
    deep += QLatin1Char('1');
    for (int i = 0; i < 80; ++i)
        deep += QLatin1Char('}');
    const Sheet sheet = parseSheet(deep, Notation::Json);
    QVERIFY(!sheet.ok);
    QVERIFY(sheet.message.contains(QStringLiteral("嵌套")));
    QVERIFY(sheet.line >= 1);

    const Sheet again = parseSheet(QStringLiteral("{\"ok\":true}"), Notation::Json);
    QVERIFY(again.ok);
}

void NotationTests::hugeTextIsRefusedBeforeParsing()
{
    const Sheet sheet = parseSheet(QString(1600000, QLatin1Char('{')), Notation::Automatic);
    QVERIFY(!sheet.ok);
    QVERIFY(sheet.message.contains(QStringLiteral("拆")));
}

void NotationTests::atomicReplaceKeepsPermissionsAndNoTemp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("config.json"));
    QVERIFY(writeBytes(path, QByteArray("old")));
    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    const QFile::Permissions permissions = QFile::permissions(path);

    QString error;
    const QByteArray bytes = QByteArray("{\"名称\":\"滕王阁\"}\n");
    QVERIFY2(commitParameterFile(path, bytes, &error), qPrintable(error));
    QCOMPARE(readBytes(path), bytes);
    QCOMPARE(QFile::permissions(path), permissions);
    QVERIFY(!directoryHasTemp(dir.path()));

    QVERIFY(!commitParameterFile(dir.filePath(QStringLiteral("missing/nope.json")), bytes, &error));
    QVERIFY(!error.isEmpty());
}

class EditorTests : public QObject
{
    Q_OBJECT

private slots:
    void toolRegistersWithoutCreatingWidgets();
    void jsonTextAndTreeStayInSync();
    void xmlTextAndTreeStayInSync();
    void iniTextAndTreeStayInSync();
    void addingAKeyUpdatesSource();
    void invalidTextKeepsSourceAndLastTree();
    void invalidEditIsWhatGetsSaved();
    void treeEditDoesNotTouchFileUntilSave();
    void utf8BomAndCrlfArePreserved();
    void invalidUtf8IsNotWrittenBack();
    void failedOpenDoesNotDiscardEditorText();
    void sessionStaysInsideDataDir();
    void backgroundParseReturnsBeforeTheTreeUpdates();
    void dualPaneFitsTypicalWindow();
};

void EditorTests::toolRegistersWithoutCreatingWidgets()
{
    ServiceFixture fixture;
    QVERIFY(fixture.dir.isValid());

    ParamCheckHighTool probe;
    QCOMPARE(probe.info().id, QStringLiteral("param-check-high"));
    QCOMPARE(probe.info().title, QStringLiteral("参数检查high"));
    ToolContext context;
    context.info = probe.info();
    context.services = fixture.services;
    QString error;
    QVERIFY2(probe.initialize(context, &error), qPrintable(error));
    QCOMPARE(probe.page(), nullptr);

    auto pages = probe.createPages();
    QCOMPARE(pages.size(), 1);
    QCOMPARE(pages.at(0).info.id, QStringLiteral("editor"));
    QCOMPARE(pages.at(0).info.title, QStringLiteral("参数检查high"));
    QVERIFY(pages.at(0).widget != nullptr);
    delete pages.at(0).widget;

    ToolRegistry registry;
    QVERIFY2(registry.add(std::make_unique<ParamCheckHighTool>(), &error), qPrintable(error));
    QVERIFY(registry.find(QStringLiteral("param-check-high")) != nullptr);
}

void EditorTests::jsonTextAndTreeStayInSync()
{
    ServiceFixture fixture;
    HighCheckPage page(fixture.services);
    page.editSourceText(QStringLiteral("{\"z\":1,\"a\":\"滕王阁\"}"));
    QVERIFY2(page.parseError().isEmpty(), qPrintable(page.parseError()));
    QVERIFY(page.treeInSync());
    QCOMPARE(static_cast<int>(page.activeFormat()), static_cast<int>(Notation::Json));
    QVERIFY(page.hasNode({QStringLiteral("z")}));
    QCOMPARE(page.nodeText({QStringLiteral("z")}), QStringLiteral("1"));
    QCOMPARE(page.nodeText({QStringLiteral("a")}), QStringLiteral("滕王阁"));

    page.editSourceText(QStringLiteral("{\"z\":2,\"b\":\"x\"}"));
    QCOMPARE(page.nodeText({QStringLiteral("z")}), QStringLiteral("2"));
    QCOMPARE(page.nodeText({QStringLiteral("b")}), QStringLiteral("x"));
    QVERIFY(!page.hasNode({QStringLiteral("a")}));

    QVERIFY(page.setNodeText({QStringLiteral("z")}, QStringLiteral("9")));
    QVERIFY(page.sourceText().contains(QStringLiteral("9")));
    QCOMPARE(page.nodeText({QStringLiteral("z")}), QStringLiteral("9"));
    QVERIFY(page.sourceText().indexOf(QStringLiteral("\"z\"")) < page.sourceText().indexOf(QStringLiteral("\"b\"")));

    auto *tree = page.findChild<QTreeView *>();
    QVERIFY(tree != nullptr);
    QVERIFY(tree->model()->setData(tree->model()->index(0, 2), QStringLiteral("8"), Qt::EditRole));
    QCOMPARE(page.nodeText({QStringLiteral("z")}), QStringLiteral("8"));
    QVERIFY(page.sourceText().contains(QStringLiteral("8")));
}

void EditorTests::xmlTextAndTreeStayInSync()
{
    ServiceFixture fixture;
    HighCheckPage page(fixture.services);
    page.editSourceText(QStringLiteral("<root id=\"1\"><name>甲</name></root>"));
    QVERIFY2(page.parseError().isEmpty(), qPrintable(page.parseError()));
    QCOMPARE(static_cast<int>(page.activeFormat()), static_cast<int>(Notation::Xml));
    QCOMPARE(page.nodeText({QStringLiteral("root"), QStringLiteral("@id")}), QStringLiteral("1"));
    QCOMPARE(page.nodeText({QStringLiteral("root"), QStringLiteral("name")}), QStringLiteral("甲"));

    page.editSourceText(QStringLiteral("<root id=\"2\"><name>乙</name></root>"));
    QCOMPARE(page.nodeText({QStringLiteral("root"), QStringLiteral("@id")}), QStringLiteral("2"));
    QCOMPARE(page.nodeText({QStringLiteral("root"), QStringLiteral("name")}), QStringLiteral("乙"));

    QVERIFY(page.setNodeText({QStringLiteral("root"), QStringLiteral("name")}, QStringLiteral("丙")));
    QVERIFY(page.sourceText().contains(QStringLiteral("丙")));
    QCOMPARE(page.nodeText({QStringLiteral("root"), QStringLiteral("name")}), QStringLiteral("丙"));
    QCOMPARE(page.nodeText({QStringLiteral("root"), QStringLiteral("@id")}), QStringLiteral("2"));
}

void EditorTests::iniTextAndTreeStayInSync()
{
    ServiceFixture fixture;
    HighCheckPage page(fixture.services);
    page.editSourceText(QStringLiteral("[server]\nport=8080\n"));
    QVERIFY2(page.parseError().isEmpty(), qPrintable(page.parseError()));
    QCOMPARE(static_cast<int>(page.activeFormat()), static_cast<int>(Notation::Ini));
    QCOMPARE(page.nodeText({QStringLiteral("server"), QStringLiteral("port")}), QStringLiteral("8080"));
    QVERIFY(page.setNodeText({QStringLiteral("server"), QStringLiteral("port")}, QStringLiteral("9090")));
    QVERIFY(page.sourceText().contains(QStringLiteral("port=9090")));
    QVERIFY(page.sourceText().contains(QStringLiteral("[server]")));
}

void EditorTests::addingAKeyUpdatesSource()
{
    ServiceFixture fixture;
    HighCheckPage page(fixture.services);
    page.editSourceText(QStringLiteral("{}"));
    QPushButton *add = nullptr;
    const auto buttons = page.findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button->text() == QStringLiteral("添加"))
            add = button;
    }
    QVERIFY(add != nullptr);
    QVERIFY(add->isEnabled());
    add->click();
    QVERIFY(page.sourceText().contains(QStringLiteral("新键")));
    QVERIFY(page.hasNode({QStringLiteral("新键")}));
}

void EditorTests::invalidTextKeepsSourceAndLastTree()
{
    ServiceFixture fixture;
    HighCheckPage page(fixture.services);
    page.editSourceText(QStringLiteral("{\"a\":1}"));
    QVERIFY(page.treeInSync());

    const QString broken = QStringLiteral("{\"a\":");
    page.editSourceText(broken);
    QCOMPARE(page.sourceText(), broken);
    QVERIFY(!page.parseError().isEmpty());
    QVERIFY(page.parseError().contains(QStringLiteral("行")));
    QVERIFY(!page.treeInSync());
    QCOMPARE(page.nodeText({QStringLiteral("a")}), QStringLiteral("1"));
    QVERIFY(!page.setNodeText({QStringLiteral("a")}, QStringLiteral("3")));
    QCOMPARE(page.sourceText(), broken);

    auto *tree = page.findChild<QTreeView *>();
    const QModelIndex value = tree->model()->index(0, 2);
    QVERIFY(!(tree->model()->flags(value) & Qt::ItemIsEditable));
    QVERIFY(!tree->model()->setData(value, QStringLiteral("4"), Qt::EditRole));
    QCOMPARE(page.sourceText(), broken);

    page.editSourceText(QStringLiteral("<root>"));
    QCOMPARE(page.sourceText(), QStringLiteral("<root>"));
    QVERIFY(page.parseError().contains(QStringLiteral("XML")));
    QVERIFY(page.parseError().contains(QStringLiteral("行")));
    QVERIFY(!page.treeInSync());
    QCOMPARE(page.nodeText({QStringLiteral("a")}), QStringLiteral("1"));
}

void EditorTests::invalidEditIsWhatGetsSaved()
{
    ServiceFixture fixture;
    const QString path = fixture.dir.filePath(QStringLiteral("bad.json"));
    QVERIFY(writeBytes(path, QByteArray("{\"a\":1}\n")));

    HighCheckPage page(fixture.services);
    QString error;
    QVERIFY2(page.openFile(path, &error), qPrintable(error));
    const QString broken = QStringLiteral("{\"a\":");
    page.editSourceText(broken);
    QVERIFY2(page.saveFile(&error), qPrintable(error));
    QCOMPARE(readText(path), broken);
    QVERIFY(!readText(path).contains(QStringLiteral("\"a\":1")));
}

void EditorTests::treeEditDoesNotTouchFileUntilSave()
{
    ServiceFixture fixture;
    const QString path = fixture.dir.filePath(QStringLiteral("cfg.json"));
    const QByteArray original = QByteArray("{\"a\":1}\n");
    QVERIFY(writeBytes(path, original));

    HighCheckPage page(fixture.services);
    QString error;
    QVERIFY2(page.openFile(path, &error), qPrintable(error));
    QVERIFY(page.setNodeText({QStringLiteral("a")}, QStringLiteral("2")));
    QVERIFY(page.sourceText().contains(QStringLiteral("2")));
    QCOMPARE(readBytes(path), original);

    QVERIFY2(page.saveFile(&error), qPrintable(error));
    const QByteArray saved = readBytes(path);
    QVERIFY(saved.contains("2"));
    QVERIFY(!saved.contains("\"a\":1"));
    QVERIFY(!directoryHasTemp(fixture.dir.path()));
}

void EditorTests::utf8BomAndCrlfArePreserved()
{
    ServiceFixture fixture;
    const QString path = fixture.dir.filePath(QStringLiteral("bom.json"));
    const QByteArray original = QByteArray("\xEF\xBB\xBF{\"a\":1}\r\n");
    QVERIFY(writeBytes(path, original));

    HighCheckPage page(fixture.services);
    QString error;
    QVERIFY2(page.openFile(path, &error), qPrintable(error));
    QCOMPARE(page.nodeText({QStringLiteral("a")}), QStringLiteral("1"));
    QVERIFY(page.setNodeText({QStringLiteral("a")}, QStringLiteral("2")));
    QVERIFY2(page.saveFile(&error), qPrintable(error));

    const QByteArray saved = readBytes(path);
    QVERIFY(saved.startsWith("\xEF\xBB\xBF"));
    QVERIFY(saved.contains("\r\n"));
    QVERIFY(saved.contains("2"));
    QTextCodec *codec = QTextCodec::codecForName("UTF-8");
    QTextCodec::ConverterState state;
    codec->toUnicode(saved.constData() + 3, saved.size() - 3, &state);
    QCOMPARE(state.invalidChars, 0);
}

void EditorTests::invalidUtf8IsNotWrittenBack()
{
    ServiceFixture fixture;
    const QString path = fixture.dir.filePath(QStringLiteral("bad-encoding.json"));
    const QByteArray original = QByteArray("{\"a\":\xff}");
    QVERIFY(writeBytes(path, original));

    HighCheckPage page(fixture.services);
    QString error;
    QVERIFY2(page.openFile(path, &error), qPrintable(error));
    QVERIFY(page.parseError().contains(QStringLiteral("UTF-8")));
    QVERIFY(!page.treeInSync());
    QVERIFY(!page.saveFile(&error));
    QVERIFY(error.contains(QStringLiteral("UTF-8")));
    QCOMPARE(readBytes(path), original);
}

void EditorTests::failedOpenDoesNotDiscardEditorText()
{
    ServiceFixture fixture;
    HighCheckPage page(fixture.services);
    page.editSourceText(QStringLiteral("{\"keep\":true}"));
    const QString before = page.sourceText();
    QString error;
    QVERIFY(!page.openFile(fixture.dir.filePath(QStringLiteral("missing.json")), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(page.sourceText(), before);
    QVERIFY(page.treeInSync());
}

void EditorTests::sessionStaysInsideDataDir()
{
    ServiceFixture fixture;
    const QString path = fixture.dir.filePath(QStringLiteral("note.json"));
    QVERIFY(writeBytes(path, QByteArray("{\"secret\":\"should-not-be-in-session\"}\n")));
    {
        HighCheckPage page(fixture.services);
        QString error;
        QVERIFY(page.openFile(path, &error));
        page.persistSession();
    }
    const QString dataDir = fixture.services.dataDir();
    QVERIFY(dataDir.contains(QStringLiteral("/tools/param-check-high")));
    const QString sessionPath = QDir(dataDir).filePath(QStringLiteral("workspace.json"));
    QVERIFY(QFileInfo::exists(sessionPath));
    const QString session = readText(sessionPath);
    QVERIFY(session.contains(QStringLiteral("lastDirectory")));
    QVERIFY(!session.contains(QStringLiteral("should-not-be-in-session")));
    QJsonParseError parseError;
    QVERIFY(!QJsonDocument::fromJson(session.toUtf8(), &parseError).isNull());
    QCOMPARE(parseError.error, QJsonParseError::NoError);
}

void EditorTests::backgroundParseReturnsBeforeTheTreeUpdates()
{
    ServiceFixture fixture;
    HighCheckPage page(fixture.services);
    QString big;
    big.reserve(4000 * 16);
    big += QLatin1Char('{');
    for (int i = 0; i < 4000; ++i) {
        if (i != 0)
            big += QLatin1Char(',');
        big += QStringLiteral("\"k");
        big += QString::number(i);
        big += QStringLiteral("\":");
        big += QString::number(i);
    }
    big += QLatin1Char('}');

    QElapsedTimer timer;
    timer.start();
    page.queueSourceParse(big);
    QVERIFY2(timer.elapsed() < 200, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(!page.treeInSync());
    QTRY_VERIFY_WITH_TIMEOUT(page.treeInSync(), 15000);
    QCOMPARE(page.nodeText({QStringLiteral("k0")}), QStringLiteral("0"));
    QCOMPARE(page.nodeText({QStringLiteral("k3999")}), QStringLiteral("3999"));
    QCOMPARE(page.sourceText().left(20), big.left(20));
}

void EditorTests::dualPaneFitsTypicalWindow()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ToolRegistry registry;
    QString error;
    QVERIFY2(registry.add(std::make_unique<ParamCheckHighTool>(), &error), qPrintable(error));
    auto platform = std::make_unique<PlatformServices>(DataPaths(dir.path() + QStringLiteral("/d")));
    MainWindow window(std::move(platform), std::move(registry));
    window.resize(1180, 760);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.selectToolById(QStringLiteral("param-check-high")));
    QTest::qWait(50);

    auto *page = window.findChild<HighCheckPage *>();
    QVERIFY(page != nullptr);
    page->editSourceText(QStringLiteral("{\"名称\":\"滕王阁\",\"port\":8080,\"items\":[1,2]}"));
    QVERIFY(page->treeInSync());
    QTest::qWait(50);

    auto *splitter = page->findChild<QSplitter *>();
    QVERIFY(splitter != nullptr);
    QVERIFY2(splitter->count() == 2, qPrintable(QString::number(splitter->count())));
    QVERIFY2(splitter->widget(0)->width() > 200, qPrintable(QString::number(splitter->widget(0)->width())));
    QVERIFY2(splitter->widget(1)->width() > 200, qPrintable(QString::number(splitter->widget(1)->width())));
    QVERIFY(page->findChild<QPlainTextEdit *>()->isVisible());
    QVERIFY(page->findChild<QTreeView *>()->isVisible());
    QVERIFY(window.grab().save(QStringLiteral("/tmp/param-check-high-1180.png"), "PNG"));

    page->editSourceText(QStringLiteral("{\"a\":"));
    QVERIFY(!page->parseError().isEmpty());
    QTest::qWait(50);
    QVERIFY(window.grab().save(QStringLiteral("/tmp/param-check-high-error.png"), "PNG"));

    window.resize(1024, 700);
    QTest::qWait(50);
    QVERIFY2(splitter->widget(0)->width() > 160, qPrintable(QString::number(splitter->widget(0)->width())));
    QVERIFY2(splitter->widget(1)->width() > 160, qPrintable(QString::number(splitter->widget(1)->width())));
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    QApplication application(argc, argv);
    int status = 0;
    {
        NotationTests tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        EditorTests tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    return status;
}

#include "check_high_tests.moc"
