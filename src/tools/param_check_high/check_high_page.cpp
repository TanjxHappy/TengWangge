#include "tools/param_check_high/check_high_page.h"

#include "shell/elided_label.h"
#include "shell/theme.h"
#include "tools/param_check_high/byte_store.h"
#include "tools/param_check_high/structure_model.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
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
    if (m_splitApplied || m_splitter == nullptr || m_splitter->width() < 480)
        return;
    m_splitApplied = true;
    if (!m_pendingSplitter.isEmpty()) {
        m_splitter->restoreState(m_pendingSplitter);
        return;
    }
    const int left = m_splitter->width() / 2;
    m_splitter->setSizes({left, qMax(1, m_splitter->width() - left)});
}

void HighCheckPage::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(6);

    auto *title = roleLabel(QStringLiteral("参数检查high"), "pageTitle");
    layout->addWidget(title);
    auto *subtitle = roleLabel(
        QStringLiteral("左边是原文，右边是结构。改任意一边，另一边会跟着变。"
                       "检查没通过时，右边停在上次成功的结构并且不能编辑。"),
        "pageSubtitle");
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_open = new QPushButton(QStringLiteral("打开"));
    m_save = new QPushButton(QStringLiteral("保存"));
    m_save->setProperty("role", QStringLiteral("primary"));
    m_saveAs = new QPushButton(QStringLiteral("另存为"));
    m_open->setAccessibleName(QStringLiteral("打开"));
    m_save->setAccessibleName(QStringLiteral("保存"));
    m_saveAs->setAccessibleName(QStringLiteral("另存为"));
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
    m_format->setMinimumWidth(108);
    toolbar->addWidget(m_format);
    m_detected = roleLabel(QStringLiteral("尚未识别"), "fieldLabel");
    m_detected->setAccessibleName(QStringLiteral("识别出的格式"));
    m_detected->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    toolbar->addWidget(m_detected);
    toolbar->addStretch(1);
    m_pathLabel = new ElidedLabel;
    m_pathLabel->setProperty("role", QStringLiteral("value"));
    m_pathLabel->setAccessibleName(QStringLiteral("当前文件"));
    m_pathLabel->setMinimumWidth(0);
    m_pathLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    toolbar->addWidget(m_pathLabel, 1);
    layout->addLayout(toolbar);

    m_banner = new QLabel;
    m_banner->setWordWrap(true);
    m_banner->setMaximumHeight(72);
    m_banner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_banner->setAccessibleName(QStringLiteral("检查结果"));
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
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_tree->setColumnWidth(0, 168);

    auto *leftPane = new QWidget;
    auto *leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);
    leftLayout->addWidget(roleLabel(QStringLiteral("源文本"), "sectionTitle"));
    leftLayout->addWidget(m_source, 1);
    leftPane->setMinimumWidth(220);

    auto *rightPane = new QWidget;
    auto *rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(4);
    rightLayout->addWidget(roleLabel(QStringLiteral("结构"), "sectionTitle"));
    rightLayout->addWidget(m_tree, 1);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    m_add = new QPushButton(QStringLiteral("添加"));
    m_addAttribute = new QPushButton(QStringLiteral("添加属性"));
    m_remove = new QPushButton(QStringLiteral("删除"));
    m_remove->setProperty("role", QStringLiteral("danger"));
    m_add->setAccessibleName(QStringLiteral("添加"));
    m_addAttribute->setAccessibleName(QStringLiteral("添加属性"));
    m_remove->setAccessibleName(QStringLiteral("删除"));
    m_type = new QComboBox;
    m_type->setAccessibleName(QStringLiteral("值的类型"));
    m_type->addItem(QStringLiteral("字符串"), static_cast<int>(NodeKind::Phrase));
    m_type->addItem(QStringLiteral("数字"), static_cast<int>(NodeKind::Quantity));
    m_type->addItem(QStringLiteral("布尔"), static_cast<int>(NodeKind::Switch));
    m_type->addItem(QStringLiteral("空"), static_cast<int>(NodeKind::Vacant));
    actions->addWidget(m_add);
    actions->addWidget(m_addAttribute);
    actions->addWidget(m_remove);
    actions->addStretch(1);
    actions->addWidget(roleLabel(QStringLiteral("类型"), "fieldLabel"));
    actions->addWidget(m_type);
    rightLayout->addLayout(actions);
    rightPane->setMinimumWidth(240);

    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->setObjectName(QStringLiteral("paramSplit"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(6);
    m_splitter->addWidget(leftPane);
    m_splitter->addWidget(rightPane);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);
    layout->addWidget(m_splitter, 1);

    auto *hint = roleLabel(
        QStringLiteral("修改会先留在窗口里。点「保存」才写入文件，并尽量保持原来的 UTF-8、BOM 和换行。"),
        "hint");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);

    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    saveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    auto *openShortcut = new QShortcut(QKeySequence::Open, this);
    openShortcut->setContext(Qt::WidgetWithChildrenShortcut);

    connect(m_open, &QPushButton::clicked, this, [this] { onOpen(); });
    connect(m_save, &QPushButton::clicked, this, [this] { onSave(); });
    connect(m_saveAs, &QPushButton::clicked, this, [this] { onSaveAs(); });
    connect(m_add, &QPushButton::clicked, this, [this] { onAdd(); });
    connect(m_addAttribute, &QPushButton::clicked, this, [this] { onAddAttribute(); });
    connect(m_remove, &QPushButton::clicked, this, [this] { onRemove(); });
    connect(openShortcut, &QShortcut::activated, this, [this] { onOpen(); });
    connect(saveShortcut, &QShortcut::activated, this, [this] { onSave(); });
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
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &,
                                                                                        const QModelIndex &) {
        refreshChrome();
    });

    setTabOrder(m_open, m_save);
    setTabOrder(m_save, m_saveAs);
    setTabOrder(m_saveAs, m_format);
    setTabOrder(m_format, m_source);
    setTabOrder(m_source, m_tree);
    setTabOrder(m_tree, m_add);
    setTabOrder(m_add, m_addAttribute);
    setTabOrder(m_addAttribute, m_remove);
    setTabOrder(m_remove, m_type);
    updatePathLabel();
}

void HighCheckPage::applyIdle()
{
    m_busy = false;
    m_treeInSync = false;
    m_error.clear();
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
    m_treeInSync = true;
    m_active = sheet.notation;
    const int nodes = countNodes(sheet.root.get());
    m_model->resetSheet(std::move(sheet), false);
    m_tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                            | QAbstractItemView::AnyKeyPressed);
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
    m_banner->setText(text);
    m_banner->setToolTip(text);
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
