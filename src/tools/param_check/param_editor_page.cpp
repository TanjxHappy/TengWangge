#include "tools/param_check/param_editor_page.h"

#include "shell/elided_label.h"
#include "shell/theme.h"
#include "tools/param_check/param_io.h"
#include "tools/param_check/param_tree_model.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QShowEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <exception>

namespace tw {
namespace {

QLabel *makeLabel(const QString &text, const char *role, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setProperty("role", QString::fromLatin1(role));
    label->setWordWrap(true);
    return label;
}

QPushButton *makeButton(const QString &text, const QString &accessibleName, const char *role, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setAccessibleName(accessibleName);
    if (role)
        button->setProperty("role", QString::fromLatin1(role));
    return button;
}

const ParamKind kScalarKinds[] = {ParamKind::String, ParamKind::Number, ParamKind::Bool, ParamKind::Null};
constexpr int kLargeTextChars = 1024 * 1024;

int asyncMinChars()
{
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("TW_PARAM_ASYNC_MIN_CHARS", &ok);
    if (ok && value >= 0)
        return value;
    return 32 * 1024;
}

int asyncDelayMs()
{
    // 只给测试用来确认解析发生在界面线程之外。正常运行时是 0。
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("TW_PARAM_ASYNC_DELAY_MS", &ok);
    if (!ok || value <= 0)
        return 0;
    return qMin(value, 5000);
}

int editorCharCount(const QPlainTextEdit *editor)
{
    if (!editor || !editor->document())
        return 0;
    return qMax(0, editor->document()->characterCount() - 1);
}

} // namespace

struct ParamAsyncParse {
    ParamDocument document;
    bool dropTreeOnFailure = false;
};

ParamEditorPage::ParamEditorPage(const ToolServices &services, QWidget *parent)
    : QWidget(parent)
    , m_services(services)
    , m_model(new ParamTreeModel(this))
    , m_parseTimer(new QTimer(this))
{
    QString dirError;
    m_dataDir = m_services.dataDir(&dirError);
    if (!m_dataDir.isEmpty())
        loadParamSession(m_dataDir, &m_lastDirectory, &m_pendingSplitter);

    buildUi();
    if (!dirError.isEmpty())
        setNotice(dirError, Notice::Error);

    m_parseTimer->setSingleShot(true);
    m_parseTimer->setInterval(200);
    connect(m_parseTimer, &QTimer::timeout, this, [this]() { beginParse(false, false); });
    connect(m_editor, &QPlainTextEdit::textChanged, this, &ParamEditorPage::onTextChanged);
    connect(m_model, &ParamTreeModel::documentEdited, this, &ParamEditorPage::onDocumentEdited);
    connect(m_model, &ParamTreeModel::editRejected, this, [this](const QString &reason) {
        m_services.showStatus(StatusState::Error, reason);
    });
    connect(m_format, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_applying)
            return;
        m_preferSuffix = false;
        m_parseTimer->stop();
        beginParse(false, false);
    });
    connect(m_type, qOverload<int>(&QComboBox::currentIndexChanged), this, &ParamEditorPage::onTypeChanged);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &, const QModelIndex &) {
        updateChrome();
    });

    auto *openShortcut = new QShortcut(QKeySequence::Open, this);
    openShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(openShortcut, &QShortcut::activated, this, &ParamEditorPage::onOpen);
    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    saveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(saveShortcut, &QShortcut::activated, this, &ParamEditorPage::onSave);

    setTabOrder(m_open, m_save);
    setTabOrder(m_save, m_format);
    setTabOrder(m_format, m_editor);
    setTabOrder(m_editor, m_tree);
    setTabOrder(m_tree, m_add);
    setTabOrder(m_add, m_addAttribute);
    setTabOrder(m_addAttribute, m_remove);
    setTabOrder(m_remove, m_type);
    updateChrome();
}

ParamEditorPage::~ParamEditorPage()
{
    ++m_parseGeneration;
    m_asyncRestart = false;
    const auto watchers = findChildren<QObject *>(QStringLiteral("paramParseWatcher"));
    for (QObject *watcher : watchers)
        disconnect(watcher, nullptr, this, nullptr);
    m_inflight.reset();
    persistSession();
}

void ParamEditorPage::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);
    layout->addWidget(makeLabel(QStringLiteral("参数检查"), "pageTitle", this));
    layout->addWidget(makeLabel(
        QStringLiteral("左侧编辑源文件，右侧查看并修改解析后的键与值。两侧修改会互相同步；只有点击「保存」才会写回文件。"),
        "pageSubtitle", this));

    m_open = makeButton(QStringLiteral("打开"), QStringLiteral("打开参数文件"), nullptr, this);
    m_save = makeButton(QStringLiteral("保存"), QStringLiteral("保存参数文件"), "primary", this);
    connect(m_open, &QPushButton::clicked, this, &ParamEditorPage::onOpen);
    connect(m_save, &QPushButton::clicked, this, &ParamEditorPage::onSave);

    auto *formatLabel = makeLabel(QStringLiteral("格式"), "fieldLabel", this);
    formatLabel->setWordWrap(false);
    m_format = new QComboBox(this);
    m_format->setAccessibleName(QStringLiteral("参数格式"));
    m_format->addItem(QStringLiteral("自动识别"), static_cast<int>(ParamFormat::Auto));
    m_format->addItem(QStringLiteral("JSON"), static_cast<int>(ParamFormat::Json));
    m_format->addItem(QStringLiteral("XML"), static_cast<int>(ParamFormat::Xml));
    m_format->addItem(QStringLiteral("INI"), static_cast<int>(ParamFormat::Ini));

    auto *detectedLabel = makeLabel(QStringLiteral("识别为"), "fieldLabel", this);
    detectedLabel->setWordWrap(false);
    m_detected = makeLabel(QStringLiteral("未识别"), "value", this);
    m_detected->setWordWrap(false);

    m_pathLabel = new ElidedLabel(this);
    m_pathLabel->setProperty("role", QStringLiteral("hint"));
    m_pathLabel->setAccessibleName(QStringLiteral("当前文件"));

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    toolbar->addWidget(m_open);
    toolbar->addWidget(m_save);
    toolbar->addSpacing(8);
    toolbar->addWidget(formatLabel);
    toolbar->addWidget(m_format);
    toolbar->addWidget(detectedLabel);
    toolbar->addWidget(m_detected);
    toolbar->addWidget(m_pathLabel, 1);
    layout->addLayout(toolbar);

    m_banner = makeLabel(QString(), "hint", this);
    m_banner->setVisible(false);
    m_banner->setAccessibleName(QStringLiteral("解析提示"));
    m_banner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_banner);

    m_editor = new QPlainTextEdit(this);
    m_editor->setObjectName(QStringLiteral("paramSource"));
    m_editor->setAccessibleName(QStringLiteral("源参数文件"));
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setPlaceholderText(
        QStringLiteral("在这里粘贴 JSON、XML 或 INI，或使用「打开」载入文件。解析失败时，这里的原文会保留。"));
    m_editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_editor->setMinimumSize(160, 140);

    m_tree = new QTreeView(this);
    m_tree->setObjectName(QStringLiteral("paramTree"));
    m_tree->setAccessibleName(QStringLiteral("解析后的结构"));
    m_tree->setModel(m_model);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                            | QAbstractItemView::AnyKeyPressed);
    m_tree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_tree->setMinimumSize(200, 140);
    m_tree->header()->setMinimumSectionSize(72);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);

    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPixelSize(Theme::workbench().fontPixelSize);
    m_editor->setFont(mono);
    m_tree->setFont(mono);

    m_add = makeButton(QStringLiteral("添加"), QStringLiteral("添加键或子项"), nullptr, this);
    m_addAttribute = makeButton(QStringLiteral("添加属性"), QStringLiteral("添加 XML 属性"), nullptr, this);
    m_remove = makeButton(QStringLiteral("删除"), QStringLiteral("删除选中项"), "danger", this);
    connect(m_add, &QPushButton::clicked, this, &ParamEditorPage::onAdd);
    connect(m_addAttribute, &QPushButton::clicked, this, &ParamEditorPage::onAddAttribute);
    connect(m_remove, &QPushButton::clicked, this, &ParamEditorPage::onRemove);

    auto *typeLabel = makeLabel(QStringLiteral("类型"), "fieldLabel", this);
    typeLabel->setWordWrap(false);
    m_type = new QComboBox(this);
    m_type->setAccessibleName(QStringLiteral("值类型"));
    m_type->addItem(QStringLiteral("字符串"));
    m_type->addItem(QStringLiteral("数字"));
    m_type->addItem(QStringLiteral("布尔"));
    m_type->addItem(QStringLiteral("空"));

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addWidget(m_add);
    actions->addWidget(m_addAttribute);
    actions->addWidget(m_remove);
    actions->addStretch(1);
    actions->addWidget(typeLabel);
    actions->addWidget(m_type);
    auto *actionBar = new QWidget(this);
    actionBar->setLayout(actions);

    auto *leftCard = new QFrame(this);
    leftCard->setProperty("role", QStringLiteral("card"));
    auto *leftLayout = new QVBoxLayout(leftCard);
    leftLayout->setContentsMargins(12, 10, 12, 10);
    leftLayout->setSpacing(8);
    leftLayout->addWidget(makeLabel(QStringLiteral("源文件"), "sectionTitle", leftCard));
    leftLayout->addWidget(m_editor, 1);

    auto *rightCard = new QFrame(this);
    rightCard->setProperty("role", QStringLiteral("card"));
    auto *rightLayout = new QVBoxLayout(rightCard);
    rightLayout->setContentsMargins(12, 10, 12, 10);
    rightLayout->setSpacing(8);
    rightLayout->addWidget(makeLabel(QStringLiteral("结构"), "sectionTitle", rightCard));
    rightLayout->addWidget(m_tree, 1);
    rightLayout->addWidget(actionBar);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setHandleWidth(8);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(leftCard);
    m_splitter->addWidget(rightCard);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);
    layout->addWidget(m_splitter, 1);

    layout->addWidget(makeLabel(
        QStringLiteral("修改文本时如果解析失败，左侧原文会保留，右侧停在上次成功的结构上且不能编辑。"
                       "打开另一个文件若解析失败，右侧会清空，避免和上一个文件混在一起。"
                       "较大的文件在后台解析，完成前不会改文件。YAML 暂不支持。"),
        "hint", this));
}

void ParamEditorPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_splitterRestored)
        return;
    m_splitterRestored = true;
    if (!m_pendingSplitter.isEmpty())
        m_splitter->restoreState(m_pendingSplitter);
}

void ParamEditorPage::persistSession()
{
    if (m_dataDir.isEmpty())
        return;
    const QByteArray splitter = m_splitter ? m_splitter->saveState() : m_pendingSplitter;
    saveParamSession(m_dataDir, m_lastDirectory, splitter);
}

bool ParamEditorPage::openFile(const QString &path, QString *error)
{
    LoadedParamFile loaded;
    QString readError;
    if (!readParamFile(path, &loaded, &readError)) {
        if (error)
            *error = readError;
        m_services.showStatus(StatusState::Error, readError);
        m_services.logError(readError);
        return false;
    }

    const QFileInfo info(path);
    m_path = info.absoluteFilePath();
    m_suffix = info.suffix();
    m_preferSuffix = true;
    m_lastDirectory = info.absolutePath();
    m_utf8Bom = loaded.utf8Bom;
    m_crlf = loaded.crlf;
    m_lossyUnedited = loaded.lossyUtf8;
    {
        QSignalBlocker blocker(m_format);
        m_format->setCurrentIndex(0);
    }
    m_applying = true;
    m_editor->setPlainText(loaded.text);
    m_applying = false;
    m_savedText = loaded.text;
    m_dirty = false;
    m_parsedOnce = true;
    m_saveBlockedReason.clear();

    if (loaded.lossyUtf8) {
        clearTree();
        setNotice(QStringLiteral("原文件不是合法的 UTF-8。左侧是替换后的预览，改正文本之前不会写回，以免损坏原始字节。右侧不显示结构。"),
                  Notice::Error);
        m_services.logError(QStringLiteral("打开了非 UTF-8 文件，未写回：") + m_path);
        updateChrome();
        persistSession();
        return true;
    }

    beginParse(false, true);
    persistSession();
    if (m_asyncBusy) {
        m_services.showStatus(StatusState::Busy, QStringLiteral("正在解析较大的文件，完成前不会改动文件。"));
        m_services.logInfo(QStringLiteral("正在解析 ") + m_path);
        updateChrome();
        return true;
    }
    if (!m_treeInSync) {
        const QString message = m_errorText.isEmpty() ? QStringLiteral("解析失败") : m_errorText;
        m_services.showStatus(StatusState::Error, message);
        m_services.logWarning(QStringLiteral("已打开，但解析失败：") + m_path);
    } else {
        m_services.showStatus(StatusState::Info, QStringLiteral("已打开 ") + m_path);
        m_services.logInfo(QStringLiteral("已打开 ") + paramFormatName(m_activeFormat) + QStringLiteral(" 文件"));
    }
    updateChrome();
    Q_UNUSED(error);
    return true;
}

bool ParamEditorPage::saveFile(QString *error)
{
    if (m_path.isEmpty()) {
        const QString message = QStringLiteral("还没有文件路径。请先选择保存位置。");
        if (error)
            *error = message;
        return false;
    }
    if (m_lossyUnedited) {
        const QString message = QStringLiteral("原文件不是合法 UTF-8，未写回，以免损坏原始字节。");
        if (error)
            *error = message;
        m_services.showStatus(StatusState::Error, message);
        m_services.logError(message);
        return false;
    }
    if (!m_saveBlockedReason.isEmpty()) {
        const QString message = m_saveBlockedReason + QStringLiteral(" 文件没有改动。");
        if (error)
            *error = message;
        m_services.showStatus(StatusState::Error, message);
        return false;
    }
    m_parseTimer->stop();
    const QString text = m_editor->toPlainText();
    if (!textIsWellFormedUtf16(text)) {
        const QString message = QStringLiteral("文本里有无法编码成 UTF-8 的字符，已取消写入，原文件未改动。");
        if (error)
            *error = message;
        m_services.showStatus(StatusState::Error, message);
        m_services.logError(message);
        return false;
    }
    if (text == m_savedText) {
        m_dirty = false;
        m_services.showStatus(StatusState::Info, QStringLiteral("没有需要保存的修改"));
        updateChrome();
        return true;
    }

    const QByteArray bytes = encodeParamText(text, m_utf8Bom, m_crlf);
    QString writeError;
    if (!writeParamFileAtomically(m_path, bytes, &writeError)) {
        if (error)
            *error = writeError;
        m_services.showStatus(StatusState::Error, writeError);
        m_services.logError(writeError);
        return false;
    }
    m_savedText = text;
    m_dirty = false;
    m_lossyUnedited = false;
    QString message = QStringLiteral("已保存");
    if (!m_treeInSync)
        message = QStringLiteral("已按左侧文本保存。当前内容未能解析，文件与左侧一致。");
    m_services.showStatus(m_treeInSync ? StatusState::Success : StatusState::Error, message);
    m_services.logSuccess(QStringLiteral("已保存 ") + m_path);
    updateChrome();
    persistSession();
    return true;
}

bool ParamEditorPage::saveFileAs(const QString &path, QString *error)
{
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("保存路径是空的");
        return false;
    }
    const QString previousPath = m_path;
    const QString previousSuffix = m_suffix;
    const QString previousDirectory = m_lastDirectory;
    const bool previousDirty = m_dirty;
    const QFileInfo info(path);
    m_path = info.absoluteFilePath();
    m_suffix = info.suffix();
    m_lastDirectory = info.absolutePath();
    m_dirty = true;
    if (saveFile(error))
        return true;
    m_path = previousPath;
    m_suffix = previousSuffix;
    m_lastDirectory = previousDirectory;
    m_dirty = previousDirty;
    updateChrome();
    return false;
}

void ParamEditorPage::editSourceText(const QString &text)
{
    m_editor->setPlainText(text);
    flushParse();
}

void ParamEditorPage::flushParse()
{
    m_parseTimer->stop();
    m_asyncRestart = false;
    beginParse(true, false);
}

QString ParamEditorPage::sourceText() const
{
    return m_editor->toPlainText();
}

bool ParamEditorPage::hasNode(const QStringList &path) const
{
    return m_model->indexForPath(path).isValid();
}

QString ParamEditorPage::nodeText(const QStringList &path) const
{
    const QModelIndex index = m_model->indexForPath(path);
    if (!index.isValid())
        return {};
    return index.sibling(index.row(), 2).data(Qt::DisplayRole).toString();
}

bool ParamEditorPage::setNodeText(const QStringList &path, const QString &value)
{
    const QModelIndex index = m_model->indexForPath(path);
    if (!index.isValid())
        return false;
    return m_model->setData(index.sibling(index.row(), 2), value, Qt::EditRole);
}

void ParamEditorPage::beginParse(bool forceSync, bool dropTreeOnFailure)
{
    const quint64 generation = ++m_parseGeneration;
    const QString text = m_editor->toPlainText();
    m_parsedOnce = true;
    if (dropTreeOnFailure)
        clearTree();

    if (text.trimmed().isEmpty()) {
        m_treeInSync = false;
        m_activeFormat = ParamFormat::Unsupported;
        if (!dropTreeOnFailure)
            m_model->setLocked(true);
        const QString message = dropTreeOnFailure
            ? QStringLiteral("文件是空的，没有可检查的参数。")
            : QStringLiteral("内容是空的。左侧原文已保留，右侧暂时不能编辑。");
        setNotice(message, Notice::Error);
        updateChrome();
        return;
    }

    const ParamFormat requested = requestedFormat();
    const QString suffix = (requested == ParamFormat::Auto && m_preferSuffix) ? m_suffix : QString();
    const bool async = !forceSync && text.size() >= asyncMinChars();
    if (async) {
        if (m_asyncBusy) {
            m_asyncRestart = true;
            m_restartDropTree = dropTreeOnFailure;
            return;
        }
        m_asyncBusy = true;
        auto payload = std::make_shared<ParamAsyncParse>();
        payload->dropTreeOnFailure = dropTreeOnFailure;
        m_inflight = payload;
        m_inflightGeneration = generation;
        setNotice(QStringLiteral("正在解析较大的内容，可以继续编辑左侧。解析完成前不会改文件。"), Notice::Busy);
        updateChrome();

        auto *watcher = new QFutureWatcher<void>(this);
        watcher->setObjectName(QStringLiteral("paramParseWatcher"));
        connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher]() {
            watcher->deleteLater();
            onAsyncFinished();
        });
        const int delayMs = asyncDelayMs();
        watcher->setFuture(QtConcurrent::run([payload, text, requested, suffix, delayMs]() {
            if (delayMs > 0)
                QThread::msleep(static_cast<unsigned long>(delayMs));
            try {
                payload->document = parseParamText(text, requested, suffix);
            } catch (const std::exception &exception) {
                payload->document.ok = false;
                payload->document.format = requested;
                payload->document.error = QStringLiteral("解析时发生内部错误，已停止。文件没有改动。（%1）")
                                              .arg(QString::fromUtf8(exception.what()));
            } catch (...) {
                payload->document.ok = false;
                payload->document.format = requested;
                payload->document.error = QStringLiteral("解析时发生内部错误，已停止。文件没有改动。");
            }
        }));
        return;
    }

    ParamDocument document = parseParamText(text, requested, suffix);
    if (generation != m_parseGeneration)
        return;
    acceptDocument(std::move(document), dropTreeOnFailure, false);
}

void ParamEditorPage::onAsyncFinished()
{
    m_asyncBusy = false;
    const quint64 generation = m_inflightGeneration;
    std::shared_ptr<ParamAsyncParse> payload = std::move(m_inflight);
    const bool restart = m_asyncRestart;
    const bool restartDrop = m_restartDropTree;
    m_asyncRestart = false;
    if (payload && generation == m_parseGeneration)
        acceptDocument(std::move(payload->document), payload->dropTreeOnFailure, true);
    if (restart)
        beginParse(false, restartDrop);
}

void ParamEditorPage::acceptDocument(ParamDocument document, bool dropTreeOnFailure, bool fromAsync)
{
    if (!document.ok || !document.root) {
        m_treeInSync = false;
        if (dropTreeOnFailure)
            clearTree();
        else
            m_model->setLocked(true);
        const QString detail = document.error.isEmpty() ? QStringLiteral("没有解析出结构。") : document.error;
        const QString tail = dropTreeOnFailure
            ? QStringLiteral(" 左侧是打开的原文，右侧已清空，避免和上一个文件混在一起。文件没有改动。")
            : QStringLiteral(" 左侧原文已保留，右侧停在上次成功的结构上，并且不能编辑。文件没有改动。");
        setNotice(detail + tail, Notice::Error);
        updateChrome();
        return;
    }

    m_activeFormat = document.format;
    m_model->setDocument(std::move(document));
    m_model->setLocked(false);
    m_treeInSync = true;
    const bool expanded = expandParsedTree();
    if (!expanded) {
        setNotice(QStringLiteral("项目较多，右侧没有自动展开。点名称左边的箭头可以逐层查看。"), Notice::Info);
    } else if (fromAsync) {
        setNotice(QString(), Notice::Clear);
        m_services.showStatus(StatusState::Success, QStringLiteral("解析完成"));
    } else {
        setNotice(QString(), Notice::Clear);
    }
    updateChrome();
}

void ParamEditorPage::clearTree()
{
    ParamDocument empty;
    empty.format = ParamFormat::Unsupported;
    empty.root = std::make_unique<ParamNode>();
    empty.root->kind = ParamKind::Document;
    m_model->setDocument(std::move(empty));
    m_model->setLocked(true);
    m_treeInSync = false;
}

bool ParamEditorPage::expandParsedTree()
{
    const int roots = m_model->rowCount();
    if (roots > 120)
        return false;
    int nested = roots;
    for (int row = 0; row < roots; ++row)
        nested += m_model->rowCount(m_model->index(row, 0));
    m_tree->expandToDepth(nested <= 400 ? 1 : 0);
    return true;
}

void ParamEditorPage::onTextChanged()
{
    if (m_applying)
        return;
    m_lossyUnedited = false;
    m_preferSuffix = false;
    m_saveBlockedReason.clear();
    m_treeInSync = false;
    m_model->setLocked(true);
    recomputeDirty();
    updateChrome();
    const bool large = editorCharCount(m_editor) >= asyncMinChars();
    m_parseTimer->setInterval(large ? 400 : 200);
    m_parseTimer->start();
}

void ParamEditorPage::onDocumentEdited()
{
    QString error;
    const QString text = serializeParam(m_model->document(), &error);
    if (!error.isEmpty()) {
        m_saveBlockedReason = error;
        setNotice(error + QStringLiteral(" 左侧文本还没改，文件也没有改动。"), Notice::Error);
        return;
    }
    m_saveBlockedReason.clear();
    QScrollBar *bar = m_editor->verticalScrollBar();
    const int scroll = bar ? bar->value() : 0;
    m_applying = true;
    m_editor->setPlainText(text);
    m_applying = false;
    if (bar)
        bar->setValue(scroll);
    m_lossyUnedited = false;
    m_treeInSync = true;
    recomputeDirty();
    setNotice(QString(), Notice::Clear);
    updateChrome();
}

void ParamEditorPage::onOpen()
{
    if (!confirmDiscard())
        return;
    const QString start = m_lastDirectory.isEmpty() ? QDir::homePath() : m_lastDirectory;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开参数文件"), start,
        QStringLiteral("参数文件 (*.json *.xml *.ini *.conf *.cfg *.properties);;"
                       "JSON (*.json);;XML (*.xml);;INI (*.ini *.conf *.cfg);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!openFile(path, &error))
        QMessageBox::warning(this, QStringLiteral("参数检查"), error);
}

void ParamEditorPage::onSave()
{
    QString error;
    bool ok = false;
    if (m_path.isEmpty())
        ok = saveAsInteractive(&error);
    else
        ok = saveFile(&error);
    if (!ok && !error.isEmpty())
        QMessageBox::warning(this, QStringLiteral("参数检查"), error);
}

void ParamEditorPage::onAdd()
{
    if (!m_model->addChild(m_tree->currentIndex()))
        m_services.showStatus(StatusState::Error, QStringLiteral("当前位置不能添加"));
    updateChrome();
}

void ParamEditorPage::onAddAttribute()
{
    if (!m_model->addAttribute(m_tree->currentIndex()))
        m_services.showStatus(StatusState::Error, QStringLiteral("请先选中 XML 元素或属性"));
    updateChrome();
}

void ParamEditorPage::onRemove()
{
    if (!m_model->removeNode(m_tree->currentIndex()))
        m_services.showStatus(StatusState::Error, QStringLiteral("这一项不能删除"));
    updateChrome();
}

void ParamEditorPage::onTypeChanged(int index)
{
    if (m_applying || index < 0 || index >= 4)
        return;
    if (!m_model->setScalarKind(m_tree->currentIndex(), kScalarKinds[index]))
        updateChrome();
}

void ParamEditorPage::updateChrome()
{
    QString pathText = m_path.isEmpty() ? QStringLiteral("尚未打开文件") : m_path;
    if (m_dirty)
        pathText += QStringLiteral("（未保存）");
    m_pathLabel->setFullText(pathText);

    if (!m_parsedOnce)
        m_detected->setText(QStringLiteral("未识别"));
    else if (m_asyncBusy && !m_treeInSync)
        m_detected->setText(QStringLiteral("正在解析"));
    else if (!m_treeInSync)
        m_detected->setText(QStringLiteral("未同步"));
    else
        m_detected->setText(paramFormatName(m_activeFormat));

    m_add->setEnabled(m_treeInSync);
    m_remove->setEnabled(m_model->canRemove(m_tree->currentIndex()));
    m_addAttribute->setEnabled(m_model->canAddAttribute(m_tree->currentIndex()));

    const ParamNode *node = m_model->nodeAt(m_tree->currentIndex());
    const bool scalar = m_treeInSync && node && m_activeFormat == ParamFormat::Json
        && (node->kind == ParamKind::String || node->kind == ParamKind::Number || node->kind == ParamKind::Bool
            || node->kind == ParamKind::Null);
    QSignalBlocker blocker(m_type);
    m_type->setEnabled(scalar);
    if (!scalar) {
        m_type->setCurrentIndex(-1);
        return;
    }
    int typeIndex = 0;
    switch (node->kind) {
    case ParamKind::Number:
        typeIndex = 1;
        break;
    case ParamKind::Bool:
        typeIndex = 2;
        break;
    case ParamKind::Null:
        typeIndex = 3;
        break;
    default:
        typeIndex = 0;
        break;
    }
    m_type->setCurrentIndex(typeIndex);
}

void ParamEditorPage::setNotice(const QString &text, Notice notice)
{
    m_errorText = notice == Notice::Error ? text : QString();
    m_banner->setText(text);
    m_banner->setVisible(!text.isEmpty());
    switch (notice) {
    case Notice::Error:
        m_services.showStatus(StatusState::Error, text);
        break;
    case Notice::Busy:
        m_services.showStatus(StatusState::Busy, text);
        break;
    case Notice::Info:
        if (text.isEmpty())
            m_services.clearStatus();
        else
            m_services.showStatus(StatusState::Info, text);
        break;
    case Notice::Clear:
        m_services.clearStatus();
        break;
    }
}

void ParamEditorPage::recomputeDirty()
{
    // 超大文本每次按键都整篇拷贝会卡住界面。先标成未保存，真正保存时再比较一次。
    if (editorCharCount(m_editor) > kLargeTextChars) {
        m_dirty = true;
        return;
    }
    m_dirty = m_editor->toPlainText() != m_savedText;
}

bool ParamEditorPage::textIsWellFormedUtf16(const QString &text) const
{
    const ushort *units = text.utf16();
    const int size = text.size();
    for (int i = 0; i < size; ++i) {
        const ushort unit = units[i];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 >= size)
                return false;
            const ushort low = units[i + 1];
            if (low < 0xDC00 || low > 0xDFFF)
                return false;
            ++i;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF)
            return false;
    }
    return true;
}

bool ParamEditorPage::confirmDiscard()
{
    if (!m_dirty && !m_lossyUnedited)
        return true;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("参数检查"));
    box.setText(QStringLiteral("当前修改还没有保存。"));
    box.setInformativeText(QStringLiteral("打开其他文件前，可以选择先保存，或者留下这份未保存的内容。"));
    auto *saveButton = box.addButton(QStringLiteral("保存"), QMessageBox::AcceptRole);
    auto *discardButton = box.addButton(QStringLiteral("不保存"), QMessageBox::DestructiveRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == discardButton)
        return true;
    if (box.clickedButton() != saveButton)
        return false;
    QString error;
    const bool ok = m_path.isEmpty() ? saveAsInteractive(&error) : saveFile(&error);
    if (!ok && !error.isEmpty())
        QMessageBox::warning(this, QStringLiteral("参数检查"), error);
    return ok;
}

bool ParamEditorPage::saveAsInteractive(QString *error)
{
    QString filter = QStringLiteral("参数文件 (*.json *.xml *.ini)");
    if (m_activeFormat == ParamFormat::Json)
        filter = QStringLiteral("JSON (*.json)");
    else if (m_activeFormat == ParamFormat::Xml)
        filter = QStringLiteral("XML (*.xml)");
    else if (m_activeFormat == ParamFormat::Ini)
        filter = QStringLiteral("INI (*.ini *.conf *.cfg)");
    const QString start = m_path.isEmpty()
        ? QDir(m_lastDirectory.isEmpty() ? QDir::homePath() : m_lastDirectory).filePath(QStringLiteral("untitled.json"))
        : m_path;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存参数文件"), start, filter);
    if (path.isEmpty())
        return false;
    return saveFileAs(path, error);
}

ParamFormat ParamEditorPage::requestedFormat() const
{
    const QVariant data = m_format->currentData();
    if (!data.isValid())
        return ParamFormat::Auto;
    return static_cast<ParamFormat>(data.toInt());
}

} // namespace tw
