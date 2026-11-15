#include "tools/param_check_high/check_high_page.h"

#include "shell/elided_label.h"
#include "shell/theme.h"
#include "tools/param_check_high/byte_store.h"
#include "tools/param_check_high/structure_model.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialog>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFormat>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <QFutureWatcher>
#include <QtConcurrent>

#include <memory>

namespace tw {
namespace {

QString utf8Refusal()
{
    return QStringLiteral(
        "这个文件不是 UTF-8 编码，里面有无法识别的字节。为避免把内容写坏，不会把它保存回去。"
        "请先用原来的编码另存为 UTF-8，再重新打开。");
}

class SourceEdit;

class LineGutter : public QWidget
{
public:
    explicit LineGutter(SourceEdit *edit);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    SourceEdit *m_edit = nullptr;
};

class SourceEdit : public QPlainTextEdit
{
public:
    explicit SourceEdit(QWidget *parent = nullptr)
        : QPlainTextEdit(parent)
        , m_gutter(new LineGutter(this))
    {
        connect(document(), &QTextDocument::blockCountChanged, this, [this](int) { updateGutterWidth(); });
        connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
            if (dy != 0)
                m_gutter->scroll(0, dy);
            else
                m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
        });
        updateGutterWidth();
    }

    void paintLineNumbers(QPaintEvent *event)
    {
        QPainter painter(m_gutter);
        painter.fillRect(event->rect(), Theme::workbench().panelAlt);
        QTextBlock block = firstVisibleBlock();
        int number = block.blockNumber();
        qreal top = blockBoundingGeometry(block).translated(contentOffset()).top();
        qreal bottom = top + blockBoundingRect(block).height();
        painter.setPen(Theme::workbench().textMuted);
        QFont numberFont = font();
        numberFont.setPointSize(qMax(8, numberFont.pointSize() - 1));
        painter.setFont(numberFont);
        while (block.isValid() && top <= event->rect().bottom()) {
            if (block.isVisible() && bottom >= event->rect().top()) {
                painter.drawText(0, qRound(top), m_gutter->width() - 4, painter.fontMetrics().height(),
                                 Qt::AlignRight, QString::number(number + 1));
            }
            block = block.next();
            ++number;
            top = bottom;
            bottom = top + blockBoundingRect(block).height();
        }
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QPlainTextEdit::resizeEvent(event);
        positionGutter();
    }

private:
    void updateGutterWidth()
    {
        const int digits = qMax(2, QString::number(qMax(1, blockCount())).size());
        const int width = fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('9'))) + 12;
        setViewportMargins(width, 0, 0, 0);
        m_gutter->setFixedWidth(width);
        positionGutter();
    }

    void positionGutter()
    {
        const QRect area = contentsRect();
        m_gutter->setGeometry(QRect(area.left(), area.top(), m_gutter->width(), area.height()));
    }

    LineGutter *m_gutter = nullptr;
};

LineGutter::LineGutter(SourceEdit *edit)
    : QWidget(edit)
    , m_edit(edit)
{
}

void LineGutter::paintEvent(QPaintEvent *event)
{
    m_edit->paintLineNumbers(event);
}

struct ParsePacket {
    int ticket = 0;
    std::shared_ptr<Sheet> sheet;
};

QLabel *roleLabel(const QString &text, const char *role)
{
    auto *label = new QLabel(text);
    label->setProperty("role", QString::fromLatin1(role));
    return label;
}

} // namespace

HighCheckPage::HighCheckPage(const ToolServices &services, QWidget *parent)
    : QWidget(parent)
    , m_services(services)
{
    QString dirError;
    m_dataDir = m_services.dataDir(&dirError);
    buildUi();
    if (!m_dataDir.isEmpty())
        loadEditorState(m_dataDir, &m_lastDirectory, &m_pendingSplitter);
    else if (!dirError.isEmpty())
        setBanner(dirError, true);
    applyIdle();
}

HighCheckPage::~HighCheckPage()
{
    persistSession();
}

void HighCheckPage::persistSession()
{
    if (m_dataDir.isEmpty() || m_splitter == nullptr)
        return;
    saveEditorState(m_dataDir, m_lastDirectory, m_splitter->saveState());
}

bool HighCheckPage::openFile(const QString &path, QString *error)
{
    LoadedText loaded;
    QString readError;
    if (!readParameterFile(path, &loaded, &readError)) {
        if (error)
            *error = readError;
        m_services.showStatus(StatusState::Error, readError);
        m_services.logError(readError);
        return false;
    }

    m_debounce->stop();
    ++m_ticket;
    m_path = QFileInfo(path).absoluteFilePath();
    m_suffix = QFileInfo(m_path).suffix();
    m_lastDirectory = QFileInfo(m_path).absolutePath();
    m_bom = loaded.bom;
    m_crlf = loaded.crlf;
    m_notUtf8 = loaded.notUtf8;
    m_quiet = true;
    m_source->setPlainText(loaded.text);
    m_quiet = false;
    m_savedText = loaded.text;
    m_dirty = false;
    updatePathLabel();
    m_services.logInfo(QStringLiteral("已打开 %1").arg(QFileInfo(m_path).fileName()));

    if (loaded.notUtf8) {
        m_busy = false;
        m_treeInSync = false;
        m_model->resetSheet(Sheet(), true);
        m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_error = utf8Refusal();
        m_errorLine = 1;
        m_errorColumn = 1;
        setBanner(QStringLiteral("需要修改：") + m_error, true);
        m_services.showStatus(StatusState::Error, m_error);
        m_services.logError(m_error);
        markLine(1, 1);
        refreshChrome();
        if (error)
            error->clear();
        return true;
    }

    if (loaded.text.size() >= 48 * 1024)
        launchParse(loaded.text);
    else
        publish(loaded.text);
    if (error)
        error->clear();
    return true;
}

bool HighCheckPage::saveFile(QString *error)
{
    if (m_path.isEmpty()) {
        const QString message = QStringLiteral("还没有选择文件。请先点「另存为」。");
        if (error)
            *error = message;
        m_services.showStatus(StatusState::Error, message);
        return false;
    }
    return saveFileAs(m_path, error);
}

bool HighCheckPage::saveFileAs(const QString &path, QString *error)
{
    if (path.isEmpty()) {
        const QString message = QStringLiteral("没有选择保存位置。");
        if (error)
            *error = message;
        return false;
    }
    const QString absolute = QFileInfo(path).absoluteFilePath();
    const bool sameFile = !m_path.isEmpty() && absolute == QFileInfo(m_path).absoluteFilePath();
    if (m_notUtf8 && sameFile) {
        const QString message = utf8Refusal();
        if (error)
            *error = message;
        setBanner(QStringLiteral("需要修改：") + message, true);
        m_services.showStatus(StatusState::Error, message);
        m_services.logError(message);
        return false;
    }

    const QByteArray bytes = encodeParameterText(m_source->toPlainText(), sameFile && m_bom, sameFile && m_crlf);
    if (!commitParameterFile(absolute, bytes, error)) {
        const QString message = error != nullptr && !error->isEmpty()
                                    ? *error
                                    : QStringLiteral("保存失败，原文件没有改动。");
        if (error && error->isEmpty())
            *error = message;
        setBanner(message, true);
        m_services.showStatus(StatusState::Error, message);
        m_services.logError(message);
        return false;
    }

    m_path = absolute;
    m_suffix = QFileInfo(absolute).suffix();
    m_lastDirectory = QFileInfo(absolute).absolutePath();
    m_savedText = m_source->toPlainText();
    m_dirty = false;
    if (!sameFile) {
        m_notUtf8 = false;
        m_bom = false;
        m_crlf = false;
    }
    updatePathLabel();
    refreshChrome();
    m_services.showStatus(StatusState::Success, QStringLiteral("已保存。"));
    m_services.logSuccess(QStringLiteral("已保存 %1").arg(QFileInfo(absolute).fileName()));
    return true;
}

void HighCheckPage::editSourceText(const QString &text)
{
    m_debounce->stop();
    m_busy = false;
    ++m_ticket;
    m_quiet = true;
    m_source->setPlainText(text);
    m_quiet = false;
    publish(text);
    m_dirty = m_source->toPlainText() != m_savedText;
    refreshChrome();
}

void HighCheckPage::queueSourceParse(const QString &text)
{
    m_debounce->stop();
    m_quiet = true;
    m_source->setPlainText(text);
    m_quiet = false;
    m_dirty = text != m_savedText;
    launchParse(text);
}

QString HighCheckPage::sourceText() const
{
    return m_source->toPlainText();
}

bool HighCheckPage::hasNode(const QStringList &path) const
{
    return !path.isEmpty() && findNode(m_model->sheet().root.get(), path) != nullptr;
}

QString HighCheckPage::nodeText(const QStringList &path) const
{
    const Node *node = findNode(m_model->sheet().root.get(), path);
    return node == nullptr ? QString() : displayedValue(node);
}

bool HighCheckPage::setNodeText(const QStringList &path, const QString &value)
{
    if (!m_treeInSync || m_model->isLocked())
        return false;
    QString error;
    if (!m_model->assignPath(path, value, &error)) {
        if (!error.isEmpty()) {
            setBanner(error, true);
            m_services.showStatus(StatusState::Error, error);
        }
        return false;
    }
    return true;
}

void HighCheckPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_splitApplied || m_splitter == nullptr || m_stack == nullptr || m_splitter->width() < 480
        || m_stack->height() < 180)
        return;
    m_splitApplied = true;
    if (!m_pendingSplitter.isEmpty())
        m_splitter->restoreState(m_pendingSplitter);
    else {
        const int width = m_splitter->width();
        const int source = width * 34 / 100;
        const int tree = width * 38 / 100;
        m_splitter->setSizes({source, tree, qMax(1, width - source - tree)});
    }
    const int height = m_stack->height();
    const int bottom = qBound(88, height / 5, 132);
    m_stack->setSizes({qMax(1, height - bottom), bottom});
}

void HighCheckPage::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(4);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(6);
    m_open = new QPushButton(QStringLiteral("打开"));
    m_save = new QPushButton(QStringLiteral("保存"));
    m_save->setProperty("role", QStringLiteral("primary"));
    m_saveAs = new QPushButton(QStringLiteral("另存为"));
    m_open->setAccessibleName(QStringLiteral("打开"));
    m_save->setAccessibleName(QStringLiteral("保存"));
    m_saveAs->setAccessibleName(QStringLiteral("另存为"));
    m_open->setToolTip(QStringLiteral("打开 JSON、XML 或 INI 参数文件。"));
    m_save->setToolTip(QStringLiteral(
        "把窗口里的文本写回当前文件。保存前不会改磁盘。"
        "会尽量保留原来的 UTF-8、BOM 和换行。不是 UTF-8 的文件不能写回原路径。"));
    m_saveAs->setToolTip(QStringLiteral("另存为一个新的 UTF-8 文件。"));
    toolbar->addWidget(m_open);
    toolbar->addWidget(m_save);
    toolbar->addWidget(m_saveAs);
    toolbar->addWidget(roleLabel(QStringLiteral("格式"), "fieldLabel"));
    m_format = new QComboBox;
    m_format->setAccessibleName(QStringLiteral("格式"));
    m_format->addItem(QStringLiteral("自动识别"), static_cast<int>(Notation::Automatic));
    m_format->addItem(QStringLiteral("JSON"), static_cast<int>(Notation::Json));
    m_format->addItem(QStringLiteral("XML"), static_cast<int>(Notation::Xml));
    m_format->addItem(QStringLiteral("INI"), static_cast<int>(Notation::Ini));
    m_format->setMinimumWidth(96);
    toolbar->addWidget(m_format);
    m_find = new QLineEdit;
    m_find->setObjectName(QStringLiteral("findEdit"));
    m_find->setAccessibleName(QStringLiteral("查找"));
    m_find->setPlaceholderText(QStringLiteral("查找名称或值"));
    m_find->setClearButtonEnabled(true);
    m_find->setMinimumWidth(96);
    m_find->setMaximumWidth(168);
    m_find->setToolTip(QStringLiteral("在结构和原文里查找。回车跳到下一处，Ctrl+F 回到这里。"));
    toolbar->addWidget(m_find);
    auto *findNext = new QPushButton(QStringLiteral("下一个"));
    auto *findPrev = new QPushButton(QStringLiteral("上一个"));
    findNext->setToolTip(QStringLiteral("跳到下一处匹配。"));
    findPrev->setToolTip(QStringLiteral("跳到上一处匹配。"));
    toolbar->addWidget(findPrev);
    toolbar->addWidget(findNext);
    auto *expandAll = new QPushButton(QStringLiteral("展开"));
    auto *collapseAll = new QPushButton(QStringLiteral("折叠"));
    expandAll->setToolTip(QStringLiteral("展开结构树。节点很多时只展开前几层。"));
    collapseAll->setToolTip(QStringLiteral("折叠结构树。"));
    toolbar->addWidget(expandAll);
    toolbar->addWidget(collapseAll);
    m_detected = roleLabel(QStringLiteral("尚未识别"), "fieldLabel");
    m_detected->setAccessibleName(QStringLiteral("识别出的格式"));
    m_detected->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    toolbar->addWidget(m_detected);
    m_pathLabel = new ElidedLabel;
    m_pathLabel->setProperty("role", QStringLiteral("value"));
    m_pathLabel->setAccessibleName(QStringLiteral("当前文件"));
    m_pathLabel->setMinimumWidth(0);
    m_pathLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    toolbar->addWidget(m_pathLabel, 1);
    layout->addLayout(toolbar);

    m_banner = new ElidedLabel;
    m_banner->setAccessibleName(QStringLiteral("检查结果"));
    m_banner->setFullText(QStringLiteral(
        "单击在原文、结构和属性之间定位。双击节点可编辑。点「保存」才写入文件。"));
    m_banner->setToolTip(QStringLiteral(
        "左边是原文，中间是结构，右边是当前节点的属性。"
        "检查没通过时，原文保留并标出行号，结构停在上次成功的结果且不能编辑。"
        "只有点「保存」才写入文件，并尽量保持原来的 UTF-8、BOM 和换行。"));
    layout->addWidget(m_banner);

    m_model = new StructureModel(this);
    auto *source = new SourceEdit;
    m_source = source;
    m_source->setObjectName(QStringLiteral("sourceEdit"));
    m_source->setAccessibleName(QStringLiteral("参数原文"));
    m_source->setPlaceholderText(QStringLiteral("在这里输入或粘贴 JSON、XML、INI。窗口里的修改不会自动写进磁盘。"));
    m_source->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_source->setTabChangesFocus(false);
    QFont font;
    font.setStyleHint(QFont::Monospace);
    font.setFamilies({QStringLiteral("WenQuanYi Micro Hei Mono"), QStringLiteral("Noto Sans Mono CJK SC"),
                      QStringLiteral("DejaVu Sans Mono")});
    font.setPointSize(10);
    m_source->setFont(font);
    m_source->viewport()->installEventFilter(this);

    m_tree = new QTreeView;
    m_tree->setObjectName(QStringLiteral("structureTree"));
    m_tree->setAccessibleName(QStringLiteral("参数结构"));
    m_tree->setModel(m_model);
    m_tree->setUniformRowHeights(true);
    m_tree->setAnimated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setMinimumSectionSize(56);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_tree->setColumnWidth(0, 108);
    m_tree->setColumnWidth(1, 64);
    m_tree->setToolTip(QStringLiteral("单击定位到原文和属性。双击打开节点详情。"));

    auto *sourcePane = new QWidget;
    sourcePane->setObjectName(QStringLiteral("sourcePane"));
    auto *sourceLayout = new QVBoxLayout(sourcePane);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->setSpacing(2);
    sourceLayout->addWidget(roleLabel(QStringLiteral("源文本"), "sectionTitle"));
    sourceLayout->addWidget(m_source, 1);
    sourcePane->setMinimumWidth(168);

    auto *treePane = new QWidget;
    treePane->setObjectName(QStringLiteral("treePane"));
    auto *treeLayout = new QVBoxLayout(treePane);
    treeLayout->setContentsMargins(0, 0, 0, 0);
    treeLayout->setSpacing(2);
    treeLayout->addWidget(roleLabel(QStringLiteral("结构"), "sectionTitle"));
    treeLayout->addWidget(m_tree, 1);
    treePane->setMinimumWidth(168);

    auto *property = new QWidget;
    property->setObjectName(QStringLiteral("propertyPane"));
    property->setMinimumWidth(188);
    auto *propertyLayout = new QVBoxLayout(property);
    propertyLayout->setContentsMargins(0, 0, 0, 0);
    propertyLayout->setSpacing(4);
    propertyLayout->addWidget(roleLabel(QStringLiteral("属性"), "sectionTitle"));

    auto *actions = new QHBoxLayout;
    actions->setSpacing(6);
    m_add = new QPushButton(QStringLiteral("添加"));
    m_remove = new QPushButton(QStringLiteral("删除"));
    m_add->setAccessibleName(QStringLiteral("添加"));
    m_remove->setAccessibleName(QStringLiteral("删除"));
    m_add->setToolTip(QStringLiteral("在当前节点下添加一项。"));
    m_remove->setToolTip(QStringLiteral("删除当前选中的节点。"));
    actions->addWidget(m_add);
    actions->addWidget(m_remove);
    actions->addStretch(1);
    propertyLayout->addLayout(actions);

    m_addAttribute = new QPushButton(QStringLiteral("添加属性"));
    m_addAttribute->setAccessibleName(QStringLiteral("添加属性"));
    m_addAttribute->setToolTip(QStringLiteral("给选中的 XML 元素加一个属性。"));
    propertyLayout->addWidget(m_addAttribute);

    auto *typeRow = new QHBoxLayout;
    typeRow->setSpacing(6);
    typeRow->addWidget(roleLabel(QStringLiteral("类型"), "fieldLabel"));
    m_type = new QComboBox;
    m_type->setAccessibleName(QStringLiteral("值的类型"));
    m_type->addItem(QStringLiteral("字符串"), static_cast<int>(NodeKind::Phrase));
    m_type->addItem(QStringLiteral("数字"), static_cast<int>(NodeKind::Quantity));
    m_type->addItem(QStringLiteral("布尔"), static_cast<int>(NodeKind::Switch));
    m_type->addItem(QStringLiteral("空"), static_cast<int>(NodeKind::Vacant));
    typeRow->addWidget(m_type, 1);
    propertyLayout->addLayout(typeRow);

    m_propPath = new ElidedLabel;
    m_propPath->setObjectName(QStringLiteral("propPath"));
    m_propPath->setProperty("role", QStringLiteral("value"));
    m_propLine = roleLabel(QStringLiteral("—"), "hint");
    m_propLine->setObjectName(QStringLiteral("propLine"));
    m_propKind = roleLabel(QStringLiteral("—"), "value");
    m_propKind->setObjectName(QStringLiteral("propKind"));
    m_propChildren = roleLabel(QStringLiteral("—"), "hint");
    m_propName = new QLineEdit;
    m_propName->setObjectName(QStringLiteral("propName"));
    m_propName->setAccessibleName(QStringLiteral("节点名称"));
    m_propValue = new QLineEdit;
    m_propValue->setObjectName(QStringLiteral("propValue"));
    m_propValue->setAccessibleName(QStringLiteral("节点的值"));
    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(6);
    form->setVerticalSpacing(4);
    form->addRow(roleLabel(QStringLiteral("路径"), "fieldLabel"), m_propPath);
    form->addRow(roleLabel(QStringLiteral("位置"), "fieldLabel"), m_propLine);
    form->addRow(roleLabel(QStringLiteral("种类"), "fieldLabel"), m_propKind);
    form->addRow(roleLabel(QStringLiteral("名称"), "fieldLabel"), m_propName);
    form->addRow(roleLabel(QStringLiteral("值"), "fieldLabel"), m_propValue);
    form->addRow(roleLabel(QStringLiteral("下级"), "fieldLabel"), m_propChildren);
    propertyLayout->addLayout(form);
    propertyLayout->addStretch(1);

    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->setObjectName(QStringLiteral("workSplit"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(6);
    m_splitter->addWidget(sourcePane);
    m_splitter->addWidget(treePane);
    m_splitter->addWidget(property);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 4);
    m_splitter->setStretchFactor(2, 3);

    auto *problemPane = new QWidget;
    problemPane->setObjectName(QStringLiteral("problemPane"));
    problemPane->setMinimumHeight(84);
    auto *problemLayout = new QVBoxLayout(problemPane);
    problemLayout->setContentsMargins(0, 0, 0, 0);
    problemLayout->setSpacing(2);
    auto *problemTitle = roleLabel(QStringLiteral("问题"), "sectionTitle");
    problemTitle->setToolTip(QStringLiteral("单击一条问题，原文会跳到对应的行。"));
    m_problems = new QListWidget;
    m_problems->setObjectName(QStringLiteral("problemList"));
    m_problems->setAccessibleName(QStringLiteral("问题列表"));
    problemLayout->addWidget(problemTitle);
    problemLayout->addWidget(m_problems, 1);

    m_stack = new QSplitter(Qt::Vertical);
    m_stack->setObjectName(QStringLiteral("stackSplit"));
    m_stack->setChildrenCollapsible(false);
    m_stack->setHandleWidth(6);
    m_stack->addWidget(m_splitter);
    m_stack->addWidget(problemPane);
    m_stack->setStretchFactor(0, 5);
    m_stack->setStretchFactor(1, 1);
    layout->addWidget(m_stack, 1);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);

    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    saveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    auto *openShortcut = new QShortcut(QKeySequence::Open, this);
    openShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);

    connect(m_open, &QPushButton::clicked, this, [this] { onOpen(); });
    connect(m_save, &QPushButton::clicked, this, [this] { onSave(); });
    connect(m_saveAs, &QPushButton::clicked, this, [this] { onSaveAs(); });
    connect(m_add, &QPushButton::clicked, this, [this] { onAdd(); });
    connect(m_addAttribute, &QPushButton::clicked, this, [this] { onAddAttribute(); });
    connect(m_remove, &QPushButton::clicked, this, [this] { onRemove(); });
    connect(expandAll, &QPushButton::clicked, this, [this] { onExpandAll(); });
    connect(collapseAll, &QPushButton::clicked, this, [this] { onCollapseAll(); });
    connect(findNext, &QPushButton::clicked, this, [this] { findStep(false); });
    connect(findPrev, &QPushButton::clicked, this, [this] { findStep(true); });
    connect(m_find, &QLineEdit::returnPressed, this, [this] { findStep(false); });
    connect(openShortcut, &QShortcut::activated, this, [this] { onOpen(); });
    connect(saveShortcut, &QShortcut::activated, this, [this] { onSave(); });
    connect(findShortcut, &QShortcut::activated, this, [this] {
        m_find->setFocus();
        m_find->selectAll();
    });
    connect(m_format, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_quiet)
            return;
        m_debounce->stop();
        ++m_ticket;
        const QString text = m_source->toPlainText();
        if (text.size() >= 48 * 1024)
            launchParse(text);
        else
            publish(text);
    });
    connect(m_type, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        onTypeChanged(index);
    });
    connect(m_propName, &QLineEdit::editingFinished, this, [this] { applyPropertyName(); });
    connect(m_propValue, &QLineEdit::editingFinished, this, [this] { applyPropertyValue(); });
    connect(m_source, &QPlainTextEdit::textChanged, this, [this] {
        if (m_quiet)
            return;
        ++m_ticket;
        m_dirty = m_source->toPlainText() != m_savedText;
        m_treeInSync = false;
        m_model->setLocked(true);
        m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        refreshChrome();
        m_debounce->start();
    });
    connect(m_source, &QPlainTextEdit::cursorPositionChanged, this, [this] { selectNodeForCursor(); });
    connect(m_debounce, &QTimer::timeout, this, [this] {
        const QString text = m_source->toPlainText();
        if (text.size() >= 48 * 1024)
            launchParse(text);
        else
            publish(text);
    });
    connect(m_model, &StructureModel::structureEdited, this, [this] { onStructureEdited(); });
    connect(m_model, &StructureModel::editRejected, this, [this](const QString &reason) {
        setBanner(reason, true);
        m_services.showStatus(StatusState::Error, reason);
    });
    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) { openNodeDialog(index); });
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current, const QModelIndex &) {
                refreshChrome();
                if (m_fromTree)
                    return;
                const Node *node = m_model->nodeAt(current);
                if (node != nullptr && node->line > 0)
                    focusSourceLine(node->line, node->column);
            });
    connect(m_problems, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const int line = item->data(Qt::UserRole).toInt();
        const int column = qMax(1, item->data(Qt::UserRole + 1).toInt());
        if (line >= 1)
            focusSourceLine(line, column);
    });

    setTabOrder(m_open, m_save);
    setTabOrder(m_save, m_saveAs);
    setTabOrder(m_saveAs, m_format);
    setTabOrder(m_format, m_find);
    setTabOrder(m_find, m_source);
    setTabOrder(m_source, m_tree);
    setTabOrder(m_tree, m_propName);
    setTabOrder(m_propName, m_propValue);
    setTabOrder(m_propValue, m_add);
    setTabOrder(m_add, m_remove);
    setTabOrder(m_remove, m_addAttribute);
    setTabOrder(m_addAttribute, m_type);
    updatePathLabel();
}

void HighCheckPage::applyIdle()
{
    m_busy = false;
    m_treeInSync = false;
    m_error.clear();
    m_errorLine = 0;
    m_errorColumn = 0;
    m_model->setLocked(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    markLine(0, 0);
    setBanner(QStringLiteral("在左边输入 JSON、XML 或 INI。只有点「保存」才会写入文件。"), false);
    m_services.clearStatus();
    refreshChrome();
}

void HighCheckPage::publish(const QString &text)
{
    if (text.trimmed().isEmpty()) {
        applyIdle();
        return;
    }
    adopt(parseSheet(text, requested(), m_suffix));
}

void HighCheckPage::launchParse(const QString &text)
{
    const int ticket = ++m_ticket;
    const Notation request = requested();
    const QString suffix = m_suffix;
    m_busy = true;
    m_treeInSync = false;
    m_model->setLocked(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    setBanner(QStringLiteral("正在分析结构，左边仍可以修改。分析完成前，右边不能编辑。"), false);
    refreshChrome();
    m_services.showStatus(StatusState::Busy, QStringLiteral("正在分析结构，窗口仍可以操作…"));
    auto *watcher = new QFutureWatcher<ParsePacket>(this);
    connect(watcher, &QFutureWatcher<ParsePacket>::finished, this, [this, watcher, ticket, text] {
        ParsePacket packet = watcher->result();
        watcher->deleteLater();
        if (ticket != m_ticket)
            return;
        if (!packet.sheet || m_source->toPlainText() != text) {
            m_busy = false;
            refreshChrome();
            return;
        }
        adopt(std::move(*packet.sheet));
    });
    watcher->setFuture(QtConcurrent::run([text, request, suffix, ticket]() {
        ParsePacket packet;
        packet.ticket = ticket;
        packet.sheet = std::make_shared<Sheet>(parseSheet(text, request, suffix));
        return packet;
    }));
}

void HighCheckPage::adopt(Sheet sheet)
{
    m_busy = false;
    if (!sheet.ok) {
        m_treeInSync = false;
        m_model->setLocked(true);
        m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_error = sheet.message;
        m_errorLine = sheet.line;
        m_errorColumn = sheet.column;
        setBanner(QStringLiteral("需要修改：") + m_error, true);
        markLine(sheet.line, sheet.column);
        m_services.showStatus(StatusState::Error, m_error);
        if (m_loggedError != m_error) {
            m_services.logWarning(m_error);
            m_loggedError = m_error;
        }
        refreshChrome();
        return;
    }

    m_error.clear();
    m_loggedError.clear();
    m_errorLine = 0;
    m_errorColumn = 0;
    m_treeInSync = true;
    m_active = sheet.notation;
    const int nodes = countNodes(sheet.root.get());
    m_model->resetSheet(std::move(sheet), false);
    m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    if (nodes > 800)
        m_tree->collapseAll();
    else
        m_tree->expandToDepth(1);
    markLine(0, 0);
    QString hint = QStringLiteral("已识别为 %1。左边和右边现在是同一份内容。").arg(notationCaption(m_active));
    if (m_dirty)
        hint += QStringLiteral(" 修改还在窗口里，点「保存」才会写入文件。");
    setBanner(hint, false);
    if (m_dirty) {
        m_services.showStatus(StatusState::Info, QStringLiteral("结构已更新。点「保存」才会写入文件。"));
    } else {
        m_services.showStatus(StatusState::Success, QStringLiteral("已识别为 %1。").arg(notationCaption(m_active)));
    }
    refreshChrome();
}

void HighCheckPage::onStructureEdited()
{
    if (m_applying)
        return;
    QString error;
    const QString text = composeSheet(m_model->sheet(), &error);
    if (!error.isEmpty()) {
        setBanner(error, true);
        m_services.showStatus(StatusState::Error, error);
        return;
    }
    m_debounce->stop();
    ++m_ticket;
    m_applying = true;
    m_quiet = true;
    m_source->setPlainText(text);
    m_quiet = false;
    m_applying = false;
    m_treeInSync = true;
    m_error.clear();
    m_errorLine = 0;
    m_errorColumn = 0;
    const Notation notation = m_active == Notation::Automatic ? requested() : m_active;
    const Sheet fresh = parseSheet(text, notation, m_suffix);
    if (fresh.ok)
        m_model->transplantLines(fresh);
    m_dirty = text != m_savedText;
    markLine(0, 0);
    setBanner(QStringLiteral("右边的修改已经反映到左边。点「保存」才会写入文件。"), false);
    m_services.showStatus(StatusState::Info, QStringLiteral("修改还在窗口里，点「保存」才会写入文件。"));
    refreshChrome();
}

void HighCheckPage::onOpen()
{
    if (!confirmDiscard())
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开参数文件"), m_lastDirectory,
        QStringLiteral("参数文件 (*.json *.xml *.ini *.cfg *.conf);;JSON (*.json);;XML (*.xml);;"
                       "INI (*.ini *.cfg *.conf);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!openFile(path, &error) && !error.isEmpty())
        setBanner(error, true);
}

void HighCheckPage::onSave()
{
    if (m_path.isEmpty()) {
        onSaveAs();
        return;
    }
    QString error;
    if (!saveFile(&error) && !error.isEmpty())
        setBanner(error, true);
}

void HighCheckPage::onSaveAs()
{
    QString suggestion = m_lastDirectory;
    if (!m_path.isEmpty())
        suggestion = m_path;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("另存为"), suggestion,
        QStringLiteral("JSON (*.json);;XML (*.xml);;INI (*.ini);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!saveFileAs(path, &error) && !error.isEmpty())
        setBanner(error, true);
}

void HighCheckPage::onAdd()
{
    if (!m_model->addChild(m_tree->currentIndex()))
        m_services.showStatus(StatusState::Error, QStringLiteral("现在不能添加。请先让左边的文本通过检查。"));
}

void HighCheckPage::onAddAttribute()
{
    if (!m_model->addAttribute(m_tree->currentIndex())) {
        m_services.showStatus(StatusState::Error, QStringLiteral("请先在右边选中一个 XML 元素，再添加属性。"));
    }
}

void HighCheckPage::onRemove()
{
    if (!m_model->removeNode(m_tree->currentIndex()))
        m_services.showStatus(StatusState::Error, QStringLiteral("这一项不能删除。"));
}

void HighCheckPage::onTypeChanged(int index)
{
    Q_UNUSED(index)
    if (m_quiet || m_model->isLocked())
        return;
    const QModelIndex current = m_tree->currentIndex();
    if (!current.isValid())
        return;
    const auto kind = static_cast<NodeKind>(m_type->currentData().toInt());
    QString error;
    if (!m_model->setScalarKind(current, kind, &error) && !error.isEmpty()) {
        setBanner(error, true);
        m_services.showStatus(StatusState::Error, error);
        refreshChrome();
    }
}

void HighCheckPage::refreshChrome()
{
    const QModelIndex current = m_tree->currentIndex();
    m_add->setEnabled(m_model->canAdd(current));
    m_addAttribute->setEnabled(m_model->canAddAttribute(current));
    m_remove->setEnabled(m_model->canRemove(current));
    m_remove->setProperty("role", m_remove->isEnabled() ? QStringLiteral("danger") : QString());
    m_remove->style()->unpolish(m_remove);
    m_remove->style()->polish(m_remove);
    m_save->setEnabled(m_dirty || !m_path.isEmpty());

    if (m_busy)
        m_detected->setText(QStringLiteral("正在分析"));
    else if (m_treeInSync)
        m_detected->setText(notationCaption(m_active));
    else if (m_error.isEmpty())
        m_detected->setText(QStringLiteral("尚未识别"));
    else
        m_detected->setText(QStringLiteral("未通过"));
    m_detected->setMinimumWidth(m_detected->fontMetrics().horizontalAdvance(m_detected->text()) + 12);

    fillProperty(current);
    refreshProblems();

    const QString kindText = current.isValid()
                                 ? m_model->data(current.sibling(current.row(), 1), Qt::DisplayRole).toString()
                                 : QString();
    const bool scalar = !m_model->isLocked() && current.isValid()
                        && (kindText == QStringLiteral("字符串") || kindText == QStringLiteral("数字")
                            || kindText == QStringLiteral("布尔") || kindText == QStringLiteral("空"));
    m_type->setEnabled(scalar);
    if (scalar) {
        const QSignalBlocker blocker(m_type);
        const int comboIndex = m_type->findData(static_cast<int>(m_model->scalarKind(current)));
        if (comboIndex >= 0)
            m_type->setCurrentIndex(comboIndex);
    }
}

void HighCheckPage::setBanner(const QString &text, bool error)
{
    m_banner->setFullText(text);
    m_banner->setProperty("role", error ? QStringLiteral("fieldLabel") : QStringLiteral("hint"));
    m_banner->style()->unpolish(m_banner);
    m_banner->style()->polish(m_banner);
}

void HighCheckPage::markLine(int line, int column)
{
    QList<QTextEdit::ExtraSelection> extras;
    if (line >= 1 && m_source->document()->blockCount() >= line) {
        QTextEdit::ExtraSelection selection;
        QColor wash = Theme::workbench().danger;
        wash.setAlpha(48);
        selection.format.setBackground(wash);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        QTextCursor cursor(m_source->document());
        cursor.movePosition(QTextCursor::Start);
        if (line > 1)
            cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor, line - 1);
        if (column > 1)
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, qMin(column - 1, cursor.block().length()));
        selection.cursor = cursor;
        extras.append(selection);

        const QTextCursor keep = m_source->textCursor();
        m_source->setTextCursor(cursor);
        m_source->ensureCursorVisible();
        m_source->setTextCursor(keep);
    }
    m_source->setExtraSelections(extras);
}

void HighCheckPage::updatePathLabel()
{
    m_pathLabel->setFullText(m_path.isEmpty() ? QStringLiteral("未打开文件") : m_path);
}

bool HighCheckPage::confirmDiscard()
{
    if (!m_dirty)
        return true;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("参数检查high"));
    box.setText(QStringLiteral("当前修改还没保存。继续打开的话，这些修改会丢掉。"));
    auto *discard = box.addButton(QStringLiteral("继续打开"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("返回"), QMessageBox::RejectRole);
    box.setDefaultButton(qobject_cast<QPushButton *>(box.buttons().value(1)));
    box.exec();
    return box.clickedButton() == discard;
}

Notation HighCheckPage::requested() const
{
    bool ok = false;
    const int value = m_format->currentData().toInt(&ok);
    if (!ok)
        return Notation::Automatic;
    switch (static_cast<Notation>(value)) {
    case Notation::Json:
    case Notation::Xml:
    case Notation::Ini:
        return static_cast<Notation>(value);
    case Notation::Automatic:
        break;
    }
    return Notation::Automatic;
}

void HighCheckPage::onExpandAll()
{
    if (countNodes(m_model->sheet().root.get()) > 800) {
        m_tree->expandToDepth(2);
        setBanner(QStringLiteral("节点较多，只展开了前几层，以免界面卡住。"), false);
        return;
    }
    m_tree->expandAll();
}

void HighCheckPage::onCollapseAll()
{
    m_tree->collapseAll();
}

void HighCheckPage::findStep(bool backward)
{
    const QString needle = m_find->text().trimmed();
    if (needle.isEmpty()) {
        setBanner(QStringLiteral("请先在查找框里输入名称或值。"), true);
        m_find->setFocus();
        return;
    }

    QList<QModelIndex> hits;
    const auto walk = [&](auto &&self, const QModelIndex &parent) -> void {
        const int rows = m_model->rowCount(parent);
        for (int row = 0; row < rows; ++row) {
            const QModelIndex nameIndex = m_model->index(row, 0, parent);
            const QString name = m_model->data(nameIndex, Qt::DisplayRole).toString();
            const QString value = m_model->data(nameIndex.sibling(nameIndex.row(), 2), Qt::DisplayRole).toString();
            if (name.contains(needle, Qt::CaseInsensitive) || value.contains(needle, Qt::CaseInsensitive))
                hits.append(nameIndex);
            self(self, nameIndex);
        }
    };
    walk(walk, QModelIndex());

    if (!hits.isEmpty()) {
        int current = -1;
        const QModelIndex selected = m_tree->currentIndex().sibling(m_tree->currentIndex().row(), 0);
        for (int i = 0; i < hits.size(); ++i) {
            if (hits.at(i) == selected) {
                current = i;
                break;
            }
        }
        int next = 0;
        if (current >= 0) {
            next = backward ? (current - 1 + hits.size()) % hits.size() : (current + 1) % hits.size();
        } else if (backward) {
            next = hits.size() - 1;
        }
        m_tree->setCurrentIndex(hits.at(next));
        m_tree->scrollTo(hits.at(next));
        setBanner(QStringLiteral("找到 %1 处，当前第 %2 处。").arg(hits.size()).arg(next + 1), false);
        return;
    }

    const QString haystack = m_source->toPlainText();
    const int from = m_source->textCursor().position();
    int pos = -1;
    if (backward)
        pos = haystack.lastIndexOf(needle, qMax(0, from - 1), Qt::CaseInsensitive);
    else
        pos = haystack.indexOf(needle, from + 1, Qt::CaseInsensitive);
    if (pos < 0) {
        pos = backward ? haystack.lastIndexOf(needle, -1, Qt::CaseInsensitive)
                       : haystack.indexOf(needle, 0, Qt::CaseInsensitive);
    }
    if (pos < 0) {
        setBanner(QStringLiteral("没有找到「%1」。").arg(needle.left(40)), true);
        return;
    }
    QTextCursor cursor = m_source->textCursor();
    cursor.setPosition(pos);
    cursor.setPosition(pos + needle.size(), QTextCursor::KeepAnchor);
    m_fromTree = true;
    m_source->setTextCursor(cursor);
    m_source->ensureCursorVisible();
    m_fromTree = false;
    setBanner(QStringLiteral("在原文里找到了「%1」。").arg(needle.left(40)), false);
}

void HighCheckPage::fillProperty(const QModelIndex &current)
{
    m_formQuiet = true;
    const Node *node = m_model->nodeAt(current);
    if (node == nullptr) {
        m_propPath->setFullText(QStringLiteral("未选中节点"));
        m_propLine->setText(QStringLiteral("—"));
        m_propKind->setText(QStringLiteral("—"));
        m_propChildren->setText(QStringLiteral("—"));
        m_propName->clear();
        m_propValue->clear();
        m_propName->setReadOnly(true);
        m_propValue->setReadOnly(true);
        m_formQuiet = false;
        return;
    }
    m_propPath->setFullText(pathText(node));
    if (node->line > 0)
        m_propLine->setText(QStringLiteral("第 %1 行第 %2 列").arg(node->line).arg(qMax(1, node->column)));
    else
        m_propLine->setText(QStringLiteral("重新识别后才有行号"));
    m_propKind->setText(nodeKindCaption(node->kind));
    m_propChildren->setText(QStringLiteral("%1 项").arg(node->childCount()));
    m_propName->setText(nodeLabel(node));
    m_propValue->setText(displayedValue(node));
    const bool locked = m_model->isLocked();
    m_propName->setReadOnly(locked || !nodeNameEditable(node));
    m_propValue->setReadOnly(locked || !nodeValueEditable(node));
    m_formQuiet = false;
}

void HighCheckPage::applyPropertyName()
{
    if (m_formQuiet || m_model->isLocked())
        return;
    const QModelIndex current = m_tree->currentIndex();
    if (!current.isValid())
        return;
    const QModelIndex nameIndex = current.sibling(current.row(), 0);
    if (m_propName->text() == m_model->data(nameIndex, Qt::DisplayRole).toString())
        return;
    if (!m_model->setData(nameIndex, m_propName->text(), Qt::EditRole))
        fillProperty(current);
}

void HighCheckPage::applyPropertyValue()
{
    if (m_formQuiet || m_model->isLocked())
        return;
    const QModelIndex current = m_tree->currentIndex();
    if (!current.isValid())
        return;
    const QModelIndex valueIndex = current.sibling(current.row(), 2);
    if (m_propValue->text() == m_model->data(valueIndex, Qt::DisplayRole).toString())
        return;
    if (!m_model->setData(valueIndex, m_propValue->text(), Qt::EditRole))
        fillProperty(current);
}

void HighCheckPage::refreshProblems()
{
    if (m_problems == nullptr)
        return;
    m_problems->clear();
    if (m_busy) {
        m_problems->addItem(QStringLiteral("正在分析结构…"));
        return;
    }
    if (!m_error.isEmpty()) {
        auto *item = new QListWidgetItem(m_error);
        item->setData(Qt::UserRole, m_errorLine);
        item->setData(Qt::UserRole + 1, m_errorColumn);
        item->setToolTip(QStringLiteral("单击后跳到原文的这一行。"));
        m_problems->addItem(item);
        return;
    }
    if (m_treeInSync) {
        const int nodes = countNodes(m_model->sheet().root.get());
        m_problems->addItem(QStringLiteral("没有问题。已识别为 %1，共 %2 项。")
                                .arg(notationCaption(m_active))
                                .arg(nodes));
        return;
    }
    m_problems->addItem(QStringLiteral("还没有可检查的内容。"));
}

void HighCheckPage::focusSourceLine(int line, int column)
{
    if (line < 1 || m_source->document()->blockCount() < line)
        return;
    m_fromTree = true;
    QTextCursor cursor(m_source->document());
    cursor.movePosition(QTextCursor::Start);
    if (line > 1)
        cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor, line - 1);
    if (column > 1) {
        cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor,
                            qMin(column - 1, qMax(0, cursor.block().length() - 1)));
    }
    m_source->setTextCursor(cursor);
    m_source->ensureCursorVisible();

    QList<QTextEdit::ExtraSelection> extras;
    QTextEdit::ExtraSelection selection;
    QColor wash = Theme::workbench().primary;
    wash.setAlpha(36);
    selection.format.setBackground(wash);
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    selection.cursor = cursor;
    extras.append(selection);
    m_source->setExtraSelections(extras);
    m_fromTree = false;
}

void HighCheckPage::selectNodeForCursor()
{
    if (m_quiet || m_fromTree || !m_treeInSync || m_model->sheet().root == nullptr)
        return;
    const int line = m_source->textCursor().blockNumber() + 1;
    const int column = m_source->textCursor().positionInBlock() + 1;
    Node *node = deepestCovering(m_model->sheet().root.get(), line, column, nullptr);
    if (node == nullptr || node == m_model->sheet().root.get())
        return;
    const QModelIndex index = m_model->indexForNode(node);
    if (!index.isValid() || index == m_tree->currentIndex())
        return;
    m_fromTree = true;
    m_tree->setCurrentIndex(index);
    m_tree->scrollTo(index);
    m_fromTree = false;
}

QString HighCheckPage::pathText(const Node *node) const
{
    QStringList parts;
    const Node *root = m_model->sheet().root.get();
    for (const Node *cursor = node; cursor != nullptr && cursor != root; cursor = cursor->parent)
        parts.prepend(nodeLabel(cursor));
    if (parts.isEmpty())
        return node == nullptr ? QString() : nodeLabel(node);
    return parts.join(QLatin1Char('/'));
}

Node *HighCheckPage::deepestCovering(Node *node, int line, int column, Node *best)
{
    if (node == nullptr)
        return best;
    if (node->line > 0 && (node->line < line || (node->line == line && node->column <= column))) {
        if (best == nullptr || node->line > best->line || (node->line == best->line && node->column >= best->column))
            best = node;
    }
    for (int i = 0; i < node->childCount(); ++i)
        best = deepestCovering(node->children[static_cast<size_t>(i)].get(), line, column, best);
    return best;
}

void HighCheckPage::openNodeDialog(const QModelIndex &index)
{
    const Node *node = m_model->nodeAt(index);
    if (node == nullptr)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("节点详情"));
    dialog.setMinimumWidth(420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *path = new ElidedLabel;
    path->setProperty("role", QStringLiteral("value"));
    path->setFullText(pathText(node));
    auto *place = roleLabel(node->line > 0 ? QStringLiteral("第 %1 行第 %2 列").arg(node->line).arg(node->column)
                                           : QStringLiteral("还没有行号"),
                            "hint");
    auto *kind = roleLabel(nodeKindCaption(node->kind), "value");
    auto *name = new QLineEdit(nodeLabel(node));
    auto *value = new QPlainTextEdit(displayedValue(node));
    value->setMinimumHeight(72);
    value->setMaximumHeight(140);
    const bool locked = m_model->isLocked();
    name->setReadOnly(locked || !nodeNameEditable(node));
    value->setReadOnly(locked || !nodeValueEditable(node));
    auto *type = new QComboBox;
    type->addItem(QStringLiteral("字符串"), static_cast<int>(NodeKind::Phrase));
    type->addItem(QStringLiteral("数字"), static_cast<int>(NodeKind::Quantity));
    type->addItem(QStringLiteral("布尔"), static_cast<int>(NodeKind::Switch));
    type->addItem(QStringLiteral("空"), static_cast<int>(NodeKind::Vacant));
    const bool scalar = isJsonScalar(node);
    type->setEnabled(!locked && scalar);
    if (scalar) {
        const int comboIndex = type->findData(static_cast<int>(node->kind));
        if (comboIndex >= 0)
            type->setCurrentIndex(comboIndex);
    }
    form->addRow(roleLabel(QStringLiteral("路径"), "fieldLabel"), path);
    form->addRow(roleLabel(QStringLiteral("位置"), "fieldLabel"), place);
    form->addRow(roleLabel(QStringLiteral("种类"), "fieldLabel"), kind);
    form->addRow(roleLabel(QStringLiteral("名称"), "fieldLabel"), name);
    if (scalar)
        form->addRow(roleLabel(QStringLiteral("类型"), "fieldLabel"), type);
    form->addRow(roleLabel(QStringLiteral("值"), "fieldLabel"), value);
    layout->addLayout(form);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *cancel = new QPushButton(locked ? QStringLiteral("关闭") : QStringLiteral("取消"));
    auto *ok = new QPushButton(QStringLiteral("确定"));
    ok->setProperty("role", QStringLiteral("primary"));
    ok->setVisible(!locked);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted || locked)
        return;
    const QModelIndex nameIndex = index.sibling(index.row(), 0);
    const QModelIndex valueIndex = index.sibling(index.row(), 2);
    if (scalar) {
        QString error;
        m_model->setScalarKind(nameIndex, static_cast<NodeKind>(type->currentData().toInt()), &error);
    }
    if (!name->isReadOnly())
        m_model->setData(nameIndex, name->text(), Qt::EditRole);
    if (!value->isReadOnly())
        m_model->setData(valueIndex, value->toPlainText(), Qt::EditRole);
}

bool HighCheckPage::eventFilter(QObject *watched, QEvent *event)
{
    if (m_source != nullptr && watched == m_source->viewport() && event->type() == QEvent::MouseButtonDblClick) {
        selectNodeForCursor();
        openNodeDialog(m_tree->currentIndex());
    }
    return QWidget::eventFilter(watched, event);
}

int HighCheckPage::countNodes(const Node *node)
{
    if (node == nullptr)
        return 0;
    int count = 1;
    for (int i = 0; i < node->childCount(); ++i)
        count += countNodes(node->children[static_cast<size_t>(i)].get());
    return count;
}

} // namespace tw
