#include "core/data_paths.h"
#include "core/platform_services.h"
#include "shell/tool_registry.h"
#include "tools/param_check/param_check_tool.h"
#include "tools/param_check/param_document.h"
#include "tools/param_check/param_editor_page.h"
#include "tools/param_check/param_io.h"

#include <QApplication>
#include <QDir>
#include <QPushButton>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCodec>

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
        if (name.contains(QLatin1String("twtmp")))
            return true;
    }
    return false;
}

class ServiceFixture {
public:
    ServiceFixture()
        : platform(DataPaths(dir.path() + QStringLiteral("/root")))
        , services(platform.servicesFor(QStringLiteral("param-check")))
    {
    }

    QTemporaryDir dir;
    PlatformServices platform;
    ToolServices services;
};

} // namespace

class ParamCodecTests : public QObject
{
    Q_OBJECT

private slots:
    void jsonKeepsKeyOrderNumbersAndUnicode();
    void jsonReportsInvalidSyntax();
    void xmlParsesAttributesTextAndDeclaration();
    void xmlReportsInvalidSyntax();
    void iniParsesSectionsCommentsAndQuotedValues();
    void iniSectionIsNotParsedAsJsonArray();
    void yamlIsRejected();
    void atomicWriteReplacesAndLeavesNoTemp();
};

void ParamCodecTests::jsonKeepsKeyOrderNumbersAndUnicode()
{
    ParamDocument document = parseParamText(
        QStringLiteral("{\"z\":1.50,\"a\":\"\\u6ed5\\u738b\\u9601\",\"n\":[true,null]}"), ParamFormat::Json);
    QVERIFY2(document.ok, qPrintable(document.error));
    QCOMPARE(document.root->kind, ParamKind::Object);
    QCOMPARE(document.root->childCount(), 3);
    QCOMPARE(document.root->children[0]->name, QStringLiteral("z"));
    QCOMPARE(document.root->children[0]->literal, QStringLiteral("1.50"));
    QCOMPARE(document.root->children[1]->value, QStringLiteral("滕王阁"));
    QCOMPARE(document.root->children[2]->children[0]->kind, ParamKind::Bool);
    QCOMPARE(document.root->children[2]->children[1]->kind, ParamKind::Null);

    const QString serialized = serializeParam(document);
    QVERIFY(serialized.indexOf(QStringLiteral("\"z\"")) >= 0);
    QVERIFY(serialized.indexOf(QStringLiteral("\"z\"")) < serialized.indexOf(QStringLiteral("\"a\"")));
    QVERIFY(serialized.contains(QStringLiteral("1.50")));
    QVERIFY(serialized.contains(QStringLiteral("滕王阁")));

    document.root->children[0]->value = QStringLiteral("2");
    document.root->children[0]->literal = QStringLiteral("2");
    const ParamDocument again = parseParamText(serializeParam(document), ParamFormat::Json);
    QVERIFY(again.ok);
    QCOMPARE(again.root->children[0]->literal, QStringLiteral("2"));
}

void ParamCodecTests::jsonReportsInvalidSyntax()
{
    const ParamDocument missing = parseParamText(QStringLiteral("{\"a\":"), ParamFormat::Json);
    QVERIFY(!missing.ok);
    QVERIFY(missing.error.contains(QStringLiteral("行")));
    QVERIFY(missing.errorLine >= 1);

    const ParamDocument duplicate = parseParamText(QStringLiteral("{\"a\":1,\"a\":2}"), ParamFormat::Json);
    QVERIFY(!duplicate.ok);
    QVERIFY(duplicate.error.contains(QStringLiteral("重复的键")));

    const ParamDocument trailing = parseParamText(QStringLiteral("[1,]"), ParamFormat::Json);
    QVERIFY(!trailing.ok);
    QVERIFY(trailing.error.contains(QStringLiteral("逗号")));
}

void ParamCodecTests::xmlParsesAttributesTextAndDeclaration()
{
    const QString source = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<root id=\"1\"><name>甲</name></root>\n");
    ParamDocument document = parseParamText(source, ParamFormat::Xml);
    QVERIFY2(document.ok, qPrintable(document.error));
    QVERIFY(document.xmlDeclaration.contains(QStringLiteral("UTF-8")));
    QCOMPARE(document.root->children[0]->name, QStringLiteral("root"));
    QCOMPARE(document.root->children[0]->children[0]->kind, ParamKind::Attribute);
    QCOMPARE(document.root->children[0]->children[0]->name, QStringLiteral("id"));
    QCOMPARE(document.root->children[0]->children[0]->value, QStringLiteral("1"));
    QCOMPARE(paramNodeValueText(document.root->children[0]->children[1].get()), QStringLiteral("甲"));

    document.root->children[0]->children[1]->children[0]->value = QStringLiteral("乙");
    const QString serialized = serializeParam(document);
    QVERIFY(serialized.contains(QStringLiteral("UTF-8")));
    QVERIFY(serialized.contains(QStringLiteral("乙")));
    QVERIFY(serialized.contains(QStringLiteral("id=\"1\"")) || serialized.contains(QStringLiteral("id='1'")));
}

void ParamCodecTests::xmlReportsInvalidSyntax()
{
    const ParamDocument document = parseParamText(QStringLiteral("<root>"), ParamFormat::Xml);
    QVERIFY(!document.ok);
    QVERIFY(document.error.contains(QStringLiteral("XML")));
    QVERIFY(document.error.contains(QStringLiteral("行")));
}

void ParamCodecTests::iniParsesSectionsCommentsAndQuotedValues()
{
    const QString source = QStringLiteral("# head\n[server]\nport=8080\nname=\"滕 王\"\n");
    ParamDocument document = parseParamText(source, ParamFormat::Ini);
    QVERIFY2(document.ok, qPrintable(document.error));
    QCOMPARE(document.root->children[0]->kind, ParamKind::Comment);
    QCOMPARE(document.root->children[0]->literal, QStringLiteral("#"));
    QCOMPARE(document.root->children[1]->name, QStringLiteral("server"));
    QCOMPARE(document.root->children[1]->children[0]->name, QStringLiteral("port"));
    QCOMPARE(document.root->children[1]->children[0]->value, QStringLiteral("8080"));
    QCOMPARE(document.root->children[1]->children[1]->value, QStringLiteral("滕 王"));

    document.root->children[1]->children[0]->value = QStringLiteral("9090");
    const QString serialized = serializeParam(document);
    QVERIFY(serialized.contains(QStringLiteral("[server]")));
    QVERIFY(serialized.contains(QStringLiteral("port=9090")));
    QVERIFY(serialized.contains(QStringLiteral("# head")));

    const ParamDocument broken = parseParamText(QStringLiteral("[server]\nnot a pair\n"), ParamFormat::Ini);
    QVERIFY(!broken.ok);
    QVERIFY(broken.error.contains(QStringLiteral("第 2 行")));
}

void ParamCodecTests::iniSectionIsNotParsedAsJsonArray()
{
    const ParamDocument ini = parseParamText(QStringLiteral("[server]\nport=8080\n"), ParamFormat::Auto);
    QVERIFY2(ini.ok, qPrintable(ini.error));
    QCOMPARE(ini.format, ParamFormat::Ini);

    const ParamDocument json = parseParamText(QStringLiteral("[1, 2]"), ParamFormat::Auto);
    QVERIFY2(json.ok, qPrintable(json.error));
    QCOMPARE(json.format, ParamFormat::Json);
    QCOMPARE(json.root->kind, ParamKind::Array);
}

void ParamCodecTests::yamlIsRejected()
{
    const ParamDocument document = parseParamText(QStringLiteral("name: value\n"), ParamFormat::Auto, QStringLiteral("yml"));
    QVERIFY(!document.ok);
    QVERIFY(document.error.contains(QStringLiteral("YAML")));
}

void ParamCodecTests::atomicWriteReplacesAndLeavesNoTemp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("config.json"));
    QVERIFY(writeBytes(path, QByteArray("old")));
    const QFile::Permissions permissions = QFile::permissions(path);

    QString error;
    const QByteArray bytes = QByteArray("{\"名称\":\"滕王阁\"}\n");
    QVERIFY2(writeParamFileAtomically(path, bytes, &error), qPrintable(error));
    QCOMPARE(readBytes(path), bytes);
    QCOMPARE(QFile::permissions(path), permissions);
    QVERIFY(!directoryHasTemp(dir.path()));

    QVERIFY2(!writeParamFileAtomically(dir.filePath(QStringLiteral("missing/nope.json")), bytes, &error),
             qPrintable(error));
    QVERIFY(!error.isEmpty());
}

class ParamEditorTests : public QObject
{
    Q_OBJECT

private slots:
    void toolIdRegistersWithoutCreatingWidgetsInInitialize();
    void jsonTextEditSyncsTreeAndTreeEditSyncsText();
    void xmlTextAndTreeStayInSync();
    void iniTextAndTreeStayInSync();
    void addingAKeyUpdatesSource();
    void invalidTextKeepsSourceAndBlocksTree();
    void invalidEditIsWhatGetsSaved();
    void treeEditDoesNotTouchFileUntilSave();
    void utf8BomAndCrlfArePreserved();
    void invalidUtf8IsNotWrittenBack();
    void failedOpenDoesNotDiscardEditorText();
    void sessionStaysInsideDataDir();
};

void ParamEditorTests::toolIdRegistersWithoutCreatingWidgetsInInitialize()
{
    ServiceFixture fixture;
    QVERIFY(fixture.dir.isValid());

    ParamCheckTool probe;
    QCOMPARE(probe.info().id, QStringLiteral("param-check"));
    QCOMPARE(probe.info().title, QStringLiteral("参数检查"));
    ToolContext context;
    context.info = probe.info();
    context.services = fixture.services;
    QString error;
    QVERIFY2(probe.initialize(context, &error), qPrintable(error));
    QCOMPARE(probe.page(), nullptr);

    auto pages = probe.createPages();
    QCOMPARE(pages.size(), 1);
    QCOMPARE(pages.at(0).info.id, QStringLiteral("editor"));
    QVERIFY(pages.at(0).widget != nullptr);
    delete pages.at(0).widget;

    ToolRegistry registry;
    QVERIFY2(registry.add(std::make_unique<ParamCheckTool>(), &error), qPrintable(error));
    QVERIFY(registry.find(QStringLiteral("param-check")) != nullptr);
}

void ParamEditorTests::jsonTextEditSyncsTreeAndTreeEditSyncsText()
{
    ServiceFixture fixture;
    ParamEditorPage page(fixture.services);
    page.editSourceText(QStringLiteral("{\"z\":1,\"a\":\"滕王阁\"}"));
    QVERIFY2(page.parseError().isEmpty(), qPrintable(page.parseError()));
    QVERIFY(page.treeInSync());
    QCOMPARE(page.activeFormat(), ParamFormat::Json);
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
}

void ParamEditorTests::xmlTextAndTreeStayInSync()
{
    ServiceFixture fixture;
    ParamEditorPage page(fixture.services);
    page.editSourceText(QStringLiteral("<root id=\"1\"><name>甲</name></root>"));
    QVERIFY2(page.parseError().isEmpty(), qPrintable(page.parseError()));
    QCOMPARE(page.activeFormat(), ParamFormat::Xml);
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

void ParamEditorTests::iniTextAndTreeStayInSync()
{
    ServiceFixture fixture;
    ParamEditorPage page(fixture.services);
    page.editSourceText(QStringLiteral("[server]\nport=8080\n"));
    QVERIFY2(page.parseError().isEmpty(), qPrintable(page.parseError()));
    QCOMPARE(page.activeFormat(), ParamFormat::Ini);
    QCOMPARE(page.nodeText({QStringLiteral("server"), QStringLiteral("port")}), QStringLiteral("8080"));
    QVERIFY(page.setNodeText({QStringLiteral("server"), QStringLiteral("port")}, QStringLiteral("9090")));
    QVERIFY(page.sourceText().contains(QStringLiteral("port=9090")));
    QVERIFY(page.sourceText().contains(QStringLiteral("[server]")));
}

void ParamEditorTests::addingAKeyUpdatesSource()
{
    ServiceFixture fixture;
    ParamEditorPage page(fixture.services);
    page.editSourceText(QStringLiteral("{}"));
    QPushButton *add = nullptr;
    for (QPushButton *button : page.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("添加"))
            add = button;
    }
    QVERIFY(add);
    QVERIFY(add->isEnabled());
    add->click();
    QVERIFY(page.sourceText().contains(QStringLiteral("新键")));
    QVERIFY(page.hasNode({QStringLiteral("新键")}));
}

void ParamEditorTests::invalidTextKeepsSourceAndBlocksTree()
{
    ServiceFixture fixture;
    ParamEditorPage page(fixture.services);
    page.editSourceText(QStringLiteral("{\"a\":1}"));
    QVERIFY(page.treeInSync());

    const QString broken = QStringLiteral("{\"a\":");
    page.editSourceText(broken);
    QCOMPARE(page.sourceText(), broken);
    QVERIFY(!page.parseError().isEmpty());
    QVERIFY(page.parseError().contains(QStringLiteral("行")));
    QVERIFY(!page.treeInSync());
    QVERIFY(!page.setNodeText({QStringLiteral("a")}, QStringLiteral("3")));
    QCOMPARE(page.sourceText(), broken);

    page.editSourceText(QStringLiteral("<root>"));
    QCOMPARE(page.sourceText(), QStringLiteral("<root>"));
    QVERIFY(page.parseError().contains(QStringLiteral("XML")));
    QVERIFY(!page.treeInSync());
}

void ParamEditorTests::invalidEditIsWhatGetsSaved()
{
    ServiceFixture fixture;
    const QString path = fixture.dir.filePath(QStringLiteral("bad.json"));
    QVERIFY(writeBytes(path, QByteArray("{\"a\":1}\n")));

    ParamEditorPage page(fixture.services);
    QString error;
    QVERIFY2(page.openFile(path, &error), qPrintable(error));
    const QString broken = QStringLiteral("{\"a\":");
    page.editSourceText(broken);
    QVERIFY2(page.saveFile(&error), qPrintable(error));
    QCOMPARE(readText(path), broken);
    QVERIFY(!readText(path).contains(QStringLiteral("\"a\":1")));
}

void ParamEditorTests::treeEditDoesNotTouchFileUntilSave()
{
    ServiceFixture fixture;
    const QString path = fixture.dir.filePath(QStringLiteral("cfg.json"));
    const QByteArray original = QByteArray("{\"a\":1}\n");
    QVERIFY(writeBytes(path, original));

    ParamEditorPage page(fixture.services);
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

void ParamEditorTests::utf8BomAndCrlfArePreserved()
{
    ServiceFixture fixture;
    const QString path = fixture.dir.filePath(QStringLiteral("bom.json"));
    const QByteArray original = QByteArray("\xEF\xBB\xBF{\"a\":1}\r\n");
    QVERIFY(writeBytes(path, original));

    ParamEditorPage page(fixture.services);
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

void ParamEditorTests::invalidUtf8IsNotWrittenBack()
{
    ServiceFixture fixture;
    const QString path = fixture.dir.filePath(QStringLiteral("bad-encoding.json"));
    const QByteArray original = QByteArray("{\"a\":\xff}");
    QVERIFY(writeBytes(path, original));

    ParamEditorPage page(fixture.services);
    QString error;
    QVERIFY2(page.openFile(path, &error), qPrintable(error));
    QVERIFY(page.parseError().contains(QStringLiteral("UTF-8")));
    QVERIFY(!page.treeInSync());
    QVERIFY(!page.saveFile(&error));
    QVERIFY(error.contains(QStringLiteral("UTF-8")));
    QCOMPARE(readBytes(path), original);
}

void ParamEditorTests::failedOpenDoesNotDiscardEditorText()
{
    ServiceFixture fixture;
    ParamEditorPage page(fixture.services);
    page.editSourceText(QStringLiteral("{\"keep\":true}"));
    const QString before = page.sourceText();
    QString error;
    QVERIFY(!page.openFile(fixture.dir.filePath(QStringLiteral("missing.json")), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(page.sourceText(), before);
    QVERIFY(page.treeInSync());
}

void ParamEditorTests::sessionStaysInsideDataDir()
{
    ServiceFixture fixture;
    const QString path = fixture.dir.filePath(QStringLiteral("note.json"));
    QVERIFY(writeBytes(path, QByteArray("{\"secret\":\"should-not-be-in-session\"}\n")));
    {
        ParamEditorPage page(fixture.services);
        QString error;
        QVERIFY(page.openFile(path, &error));
        page.persistSession();
    }
    const QString dataDir = fixture.services.dataDir();
    QVERIFY(dataDir.contains(QStringLiteral("/tools/param-check")));
    const QString sessionPath = QDir(dataDir).filePath(QStringLiteral("session.json"));
    QVERIFY(QFileInfo::exists(sessionPath));
    const QString session = readText(sessionPath);
    QVERIFY(session.contains(QStringLiteral("lastDirectory")));
    QVERIFY(!session.contains(QStringLiteral("should-not-be-in-session")));
    QJsonParseError parseError;
    QVERIFY(!QJsonDocument::fromJson(session.toUtf8(), &parseError).isNull());
    QCOMPARE(parseError.error, QJsonParseError::NoError);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    QApplication application(argc, argv);
    int status = 0;
    {
        ParamCodecTests tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        ParamEditorTests tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    return status;
}

#include "param_check_tests.moc"
