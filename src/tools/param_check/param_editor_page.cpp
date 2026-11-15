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
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QShowEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

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

} // namespace

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
        setBanner(dirError, true);

    m_parseTimer->setSingleShot(true);
    m_parseTimer->setInterval(200);
    connect(m_parseTimer, &QTimer::timeout, this, &ParamEditorPage::applyParse);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &ParamEditorPage::onTextChanged);
    connect(m_model, &ParamTreeModel::documentEdited, this, &ParamEditorPage::onDocumentEdited);
    connect(m_model, &ParamTreeModel::editRejected, this, [this](const QString &reason) {
        m_services.showStatus(StatusState::Error, reason);
    });
    connect(m_format, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_applying)
            return;
        m_preferSuffix = false;
        flushParse();
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
    layout->addWidget(m_banner);

    m_editor = new QPlainTextEdit(this);
    m_editor->setObjectName(QStringLiteral("paramSource"));
    m_editor->setAccessibleName(QStringLiteral("源参数文件"));
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setPlaceholderText(
        QStringLiteral("在这里粘贴 JSON、XML 或 INI，或使用「打开」载入文件。解析失败时，这里的原文会保留。"));
    m_editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_editor->setMinimumWidth(80);

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
    m_tree->setMinimumWidth(80);
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
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(leftCard);
    m_splitter->addWidget(rightCard);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);
    layout->addWidget(m_splitter, 1);

    layout->addWidget(makeLabel(
        QStringLiteral("解析失败时保留左侧原文，并暂停右侧编辑，避免把上一次的结构写回去。YAML 暂不支持。"),
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

    if (loaded.lossyUtf8) {
        m_treeInSync = false;
        m_model->setLocked(true);
        setBanner(QStringLiteral("原文件不是合法的 UTF-8。左侧是替换后的预览，改正文本之前不会写回，以免损坏原始字节。右侧已暂停同步。"),
                  true);
        m_services.logError(QStringLiteral("打开了非 UTF-8 文件，未写回：") + m_path);
        updateChrome();
        persistSession();
        return true;
    }

    applyParse();
    if (!m_treeInSync) {
        const QString message = m_errorText.isEmpty() ? QStringLiteral("解析失败") : m_errorText;
        m_services.showStatus(StatusState::Error, message);
        m_services.logWarning(QStringLiteral("已打开，但解析失败：") + m_path);
    } else {
        m_services.showStatus(StatusState::Info, QStringLiteral("已打开 ") + m_path);
        m_services.logInfo(QStringLiteral("已打开 ") + paramFormatName(m_activeFormat) + QStringLiteral(" 文件"));
    }
    updateChrome();
    persistSession();
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
    m_parseTimer->stop();
    if (!m_dirty) {
        m_services.showStatus(StatusState::Info, QStringLiteral("没有需要保存的修改"));
        return true;
    }

    const QByteArray bytes = encodeParamText(m_editor->toPlainText(), m_utf8Bom, m_crlf);
    QString writeError;
    if (!writeParamFileAtomically(m_path, bytes, &writeError)) {
        if (error)
            *error = writeError;
        m_services.showStatus(StatusState::Error, writeError);
        m_services.logError(writeError);
        return false;
    }
    m_savedText = m_editor->toPlainText();
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
    const QFileInfo info(path);
    m_path = info.absoluteFilePath();
    m_suffix = info.suffix();
    m_lastDirectory = info.absolutePath();
    m_dirty = true;
    return saveFile(error);
}

void ParamEditorPage::editSourceText(const QString &text)
{
    m_editor->setPlainText(text);
    flushParse();
}

void ParamEditorPage::flushParse()
{
    m_parseTimer->stop();
    applyParse();
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

void ParamEditorPage::applyParse()
{
    const QString text = m_editor->toPlainText();
    m_parsedOnce = true;
    if (text.trimmed().isEmpty()) {
        m_treeInSync = false;
        m_activeFormat = ParamFormat::Unsupported;
        m_model->setLocked(true);
        setBanner(QStringLiteral("内容是空的，没有可解析的参数。左侧原文已保留。"), true);
        updateChrome();
        return;
    }

    const ParamFormat requested = requestedFormat();
    const QString suffix = (requested == ParamFormat::Auto && m_preferSuffix) ? m_suffix : QString();
    ParamDocument document = parseParamText(text, requested, suffix);
    if (!document.ok) {
        m_treeInSync = false;
        m_model->setLocked(true);
        setBanner(document.error + QStringLiteral(" 左侧原文已保留，右侧暂停同步。"), true);
        updateChrome();
        return;
    }

    m_activeFormat = document.format;
    m_model->setDocument(std::move(document));
    m_model->setLocked(false);
    m_treeInSync = true;
    m_tree->expandToDepth(1);
    setBanner({}, false);
    updateChrome();
}

void ParamEditorPage::onTextChanged()
{
    if (m_applying)
        return;
    m_lossyUnedited = false;
    m_preferSuffix = false;
    m_treeInSync = false;
    m_model->setLocked(true);
    recomputeDirty();
    updateChrome();
    m_parseTimer->start();
}

void ParamEditorPage::onDocumentEdited()
{
    QString error;
    const QString text = serializeParam(m_model->document(), &error);
    if (!error.isEmpty()) {
        setBanner(error, true);
        m_services.showStatus(StatusState::Error, error);
        return;
    }
    m_applying = true;
    m_editor->setPlainText(text);
    m_applying = false;
    m_lossyUnedited = false;
    m_treeInSync = true;
    recomputeDirty();
    setBanner({}, false);
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

void ParamEditorPage::setBanner(const QString &text, bool error)
{
    m_errorText = text;
    m_banner->setText(text);
    m_banner->setVisible(!text.isEmpty());
    if (text.isEmpty())
        m_services.clearStatus();
    else if (error)
        m_services.showStatus(StatusState::Error, text);
}

void ParamEditorPage::recomputeDirty()
{
    m_dirty = m_editor->toPlainText() != m_savedText;
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
