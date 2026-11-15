#include "tools/core_analyze_high/high_tool.h"

#include <QAbstractItemView>
#include <QClipboard>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QVariant>

namespace tw {
namespace {

// 换行标签在宽度未知时的 minimumSizeHint 会非常高。先按一行计，
// 显示之后再按实际宽度把最小高度收到能放下全文。
class BoundLabel : public QLabel
{
public:
    explicit BoundLabel(const QString &text, QWidget *parent = nullptr)
        : QLabel(text, parent)
    {
        setWordWrap(true);
        setMinimumWidth(0);
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, fontMetrics().lineSpacing() + 4);
    }
};

BoundLabel *makeLabel(const QString &text, const char *role)
{
    auto *label = new BoundLabel(text);
    label->setProperty("role", QString::fromLatin1(role));
    return label;
}

QFrame *makeCard()
{
    auto *frame = new QFrame;
    frame->setProperty("role", QStringLiteral("card"));
    frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    return frame;
}

void plainButton(QPushButton *button)
{
    button->setAutoDefault(false);
    button->setDefault(false);
}

QWidget *pathRow(QLabel *label, QLineEdit *edit, QPushButton *browse, QPushButton *extra = nullptr)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    label->setMinimumWidth(72);
    edit->setMinimumWidth(0);
    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(label);
    layout->addWidget(edit, 1);
    layout->addWidget(browse);
    if (extra)
        layout->addWidget(extra);
    return row;
}

QString startDirectory(const QLineEdit *edit)
{
    const QFileInfo info(edit->text().trimmed());
    if (info.exists())
        return info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    if (!info.absolutePath().isEmpty() && QFileInfo::exists(info.absolutePath()))
        return info.absolutePath();
    return QDir::homePath();
}

QString sliceDocument(const QString &document, const QString &marker, const QString &next)
{
    const int from = document.indexOf(marker);
    if (from < 0)
        return {};
    int to = document.size();
    if (!next.isEmpty()) {
        const int found = document.indexOf(next, from + marker.size());
        if (found >= 0)
            to = found;
    }
    return document.mid(from, to - from).trimmed();
}

QString stepDetailText(const TriageStep &step, const QString &section)
{
    QString detail;
    detail += step.heading;
    detail += QLatin1Char('\n');
    if (!step.purpose.isEmpty()) {
        detail += step.purpose;
        detail += QLatin1Char('\n');
    }
    if (!step.commandLine.isEmpty()) {
        detail += QStringLiteral("\n命令\n");
        detail += step.commandLine;
        detail += QLatin1Char('\n');
    }
    if (!step.situation.isEmpty()) {
        detail += QStringLiteral("\n本次情况\n");
        detail += step.situation;
        detail += QLatin1Char('\n');
    }
    if (!step.excerpt.isEmpty()) {
        detail += QStringLiteral("\n关键输出\n");
        detail += step.excerpt;
        detail += QLatin1Char('\n');
    }
    if (!step.meaning.isEmpty()) {
        detail += QStringLiteral("\n中文释义\n");
        detail += step.meaning;
        detail += QLatin1Char('\n');
    }
    if (step.commandLine.isEmpty() && !section.isEmpty()) {
        detail += QLatin1Char('\n');
        detail += section;
    }
    return detail.trimmed();
}

} // namespace

TriageThread::TriageThread(TriageInput input, ToolLocator locator, ArgvHostFactory factory,
                           QObject *parent)
    : QThread(parent)
    , m_input(std::move(input))
    , m_locator(std::move(locator))
    , m_factory(std::move(factory))
{
}

void TriageThread::run()
{
    std::unique_ptr<ArgvHost> host;
    if (m_factory)
        host = m_factory(&m_stop);
    if (!host)
        host = std::make_unique<SystemArgvHost>(&m_stop);
    m_outcome = CoreTriage::run(m_input, *host, m_locator, &m_stop,
                                [this](const QString &text) { emit note(text); });
}

HighAnalyzePage::HighAnalyzePage(const ToolServices &services, QWidget *dialogParent, QWidget *parent)
    : QWidget(parent)
    , m_services(services)
    , m_dialogParent(dialogParent)
{
    // 四块可拖动区域：步骤表、结论、本步详情、底部说明。
    // 分栏只借鉴调试器里常见的「列表 + 详情 + 底部日志」，不搬那些项目的代码。
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);

    m_coreEdit = new QLineEdit;
    m_coreEdit->setObjectName(QStringLiteral("coreFileEdit"));
    m_coreEdit->setPlaceholderText(QStringLiteral("选择 core 文件"));
    m_coreEdit->setAccessibleName(QStringLiteral("core 文件"));
    m_coreEdit->setClearButtonEnabled(true);
    m_coreEdit->setToolTip(QStringLiteral("选择 Linux core 文件。程序文件和调试符号在「条件」里，可以留空。"));

    auto *coreBrowse = new QPushButton(QStringLiteral("浏览"));
    plainButton(coreBrowse);
    coreBrowse->setAccessibleName(QStringLiteral("浏览 core 文件"));

    auto *conditions = new QPushButton(QStringLiteral("条件"));
    plainButton(conditions);
    conditions->setObjectName(QStringLiteral("conditionButton"));
    conditions->setAccessibleName(QStringLiteral("分析条件"));
    conditions->setToolTip(QStringLiteral("可选：程序文件与调试符号。留空也可以分析。"));

    m_run = new QPushButton(QStringLiteral("开始分析"));
    plainButton(m_run);
    m_run->setObjectName(QStringLiteral("runButton"));
    m_run->setProperty("role", QStringLiteral("primary"));
    m_run->setAccessibleName(QStringLiteral("开始分析"));

    m_stop = new QPushButton(QStringLiteral("取消"));
    plainButton(m_stop);
    m_stop->setObjectName(QStringLiteral("stopButton"));
    m_stop->setAccessibleName(QStringLiteral("取消分析"));

    m_save = new QPushButton(QStringLiteral("导出说明"));
    plainButton(m_save);
    m_save->setObjectName(QStringLiteral("saveButton"));
    m_save->setAccessibleName(QStringLiteral("导出分析说明"));

    m_bar = new QProgressBar;
    m_bar->setObjectName(QStringLiteral("progressBar"));
    m_bar->setAccessibleName(QStringLiteral("分析进度"));
    m_bar->setRange(0, 1);
    m_bar->setValue(0);
    m_bar->setTextVisible(false);
    m_bar->setFixedWidth(72);

    m_progress = new QLabel(QStringLiteral("尚未分析"));
    m_progress->setProperty("role", QStringLiteral("hint"));
    m_progress->setObjectName(QStringLiteral("progressLabel"));
    m_progress->setWordWrap(false);
    m_progress->setMinimumWidth(72);
    m_progressFull = m_progress->text();

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(6);
    auto *coreName = makeLabel(QStringLiteral("core"), "fieldLabel");
    coreName->setToolTip(QStringLiteral("程序异常退出时留下的 core 文件"));
    toolbar->addWidget(coreName);
    toolbar->addWidget(m_coreEdit, 1);
    toolbar->addWidget(coreBrowse);
    toolbar->addWidget(conditions);
    toolbar->addWidget(m_run);
    toolbar->addWidget(m_stop);
    toolbar->addWidget(m_save);
    toolbar->addWidget(m_bar);
    toolbar->addWidget(m_progress);
    layout->addLayout(toolbar);

    m_conditions = new QDialog(this);
    m_conditions->setObjectName(QStringLiteral("conditionDialog"));
    m_conditions->setWindowTitle(QStringLiteral("分析条件"));
    m_conditions->setModal(true);
    auto *conditionLayout = new QVBoxLayout(m_conditions);
    conditionLayout->setContentsMargins(12, 12, 12, 12);
    conditionLayout->setSpacing(8);
    m_programEdit = new QLineEdit(m_conditions);
    m_programEdit->setObjectName(QStringLiteral("programFileEdit"));
    m_programEdit->setPlaceholderText(QStringLiteral("可选"));
    m_programEdit->setAccessibleName(QStringLiteral("程序文件"));
    m_programEdit->setClearButtonEnabled(true);
    m_programEdit->setToolTip(QStringLiteral("生成这份 core 的程序。对不上也可以先不填。"));
    auto *programBrowse = new QPushButton(QStringLiteral("浏览"), m_conditions);
    plainButton(programBrowse);
    programBrowse->setAccessibleName(QStringLiteral("浏览程序文件"));

    m_debugEdit = new QLineEdit(m_conditions);
    m_debugEdit->setObjectName(QStringLiteral("debugPathEdit"));
    m_debugEdit->setPlaceholderText(QStringLiteral("可选，文件或目录"));
    m_debugEdit->setAccessibleName(QStringLiteral("调试符号"));
    m_debugEdit->setClearButtonEnabled(true);
    m_debugEdit->setToolTip(QStringLiteral("调试符号用来把地址翻译成步骤名。没有也可以分析。"));
    auto *debugFile = new QPushButton(QStringLiteral("文件"), m_conditions);
    plainButton(debugFile);
    debugFile->setAccessibleName(QStringLiteral("浏览调试符号文件"));
    auto *debugDir = new QPushButton(QStringLiteral("目录"), m_conditions);
    plainButton(debugDir);
    debugDir->setAccessibleName(QStringLiteral("浏览调试符号目录"));

    conditionLayout->addWidget(pathRow(makeLabel(QStringLiteral("程序文件"), "fieldLabel"),
                                       m_programEdit, programBrowse));
    conditionLayout->addWidget(pathRow(makeLabel(QStringLiteral("调试符号"), "fieldLabel"),
                                       m_debugEdit, debugFile, debugDir));
    auto *conditionButtons = new QHBoxLayout;
    auto *conditionOk = new QPushButton(QStringLiteral("确定"), m_conditions);
    conditionOk->setObjectName(QStringLiteral("conditionOk"));
    conditionOk->setDefault(true);
    auto *conditionCancel = new QPushButton(QStringLiteral("取消"), m_conditions);
    plainButton(conditionCancel);
    conditionButtons->addStretch(1);
    conditionButtons->addWidget(conditionOk);
    conditionButtons->addWidget(conditionCancel);
    conditionLayout->addLayout(conditionButtons);
    m_conditions->resize(520, 160);
    connect(conditionOk, &QPushButton::clicked, m_conditions, &QDialog::accept);
    connect(conditionCancel, &QPushButton::clicked, m_conditions, &QDialog::reject);

    m_vertical = new QSplitter(Qt::Vertical);
    m_vertical->setObjectName(QStringLiteral("workspaceSplit"));
    m_vertical->setChildrenCollapsible(false);
    m_vertical->setHandleWidth(6);
    m_columns = new QSplitter(Qt::Horizontal);
    m_columns->setObjectName(QStringLiteral("columnSplit"));
    m_columns->setChildrenCollapsible(false);
    m_columns->setHandleWidth(6);
    m_detailSplit = new QSplitter(Qt::Horizontal);
    m_detailSplit->setObjectName(QStringLiteral("detailSplit"));
    m_detailSplit->setChildrenCollapsible(false);
    m_detailSplit->setHandleWidth(6);

    auto *stepsCard = makeCard();
    auto *stepsLayout = new QVBoxLayout(stepsCard);
    stepsLayout->setContentsMargins(8, 6, 8, 6);
    stepsLayout->setSpacing(4);
    auto *stepsTitle = makeLabel(QStringLiteral("分析步骤"), "sectionTitle");
    stepsTitle->setObjectName(QStringLiteral("stepTitle"));
    stepsLayout->addWidget(stepsTitle);
    m_steps = new QTableWidget(0, 2);
    m_steps->setObjectName(QStringLiteral("stepTable"));
    m_steps->setAccessibleName(QStringLiteral("分析步骤"));
    m_steps->setHorizontalHeaderLabels({QStringLiteral("步骤"), QStringLiteral("本次情况")});
    m_steps->verticalHeader()->setVisible(false);
    m_steps->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_steps->setSelectionMode(QAbstractItemView::SingleSelection);
    m_steps->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_steps->setShowGrid(true);
    m_steps->setWordWrap(false);
    m_steps->setAlternatingRowColors(true);
    m_steps->setFocusPolicy(Qt::StrongFocus);
    m_steps->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_steps->setMinimumWidth(140);
    m_steps->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_steps->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_steps->setColumnWidth(1, 96);
    m_steps->horizontalHeader()->setStretchLastSection(false);
    m_steps->horizontalHeader()->setToolTip(QStringLiteral("按实际执行顺序排列"));
    m_steps->setToolTip(QStringLiteral("单击定位到下方说明，双击或按回车弹出全文，右键可复制命令。"));
    m_steps->setContextMenuPolicy(Qt::CustomContextMenu);
    m_steps->installEventFilter(this);
    stepsLayout->addWidget(m_steps, 1);

    auto *conclusion = makeCard();
    auto *conclusionLayout = new QVBoxLayout(conclusion);
    conclusionLayout->setContentsMargins(8, 6, 8, 6);
    conclusionLayout->setSpacing(4);
    auto *conclusionTitle = makeLabel(QStringLiteral("结论"), "sectionTitle");
    conclusionTitle->setObjectName(QStringLiteral("conclusionTitle"));
    conclusionTitle->setToolTip(QStringLiteral("单击某一项跳到相关步骤，双击弹出全文。"));
    conclusionLayout->addWidget(conclusionTitle);

    auto addValue = [&](const QString &name, QLabel **slot, const QString &objectName) {
        auto *value = makeLabel(QStringLiteral("尚未分析"), "value");
        value->setObjectName(objectName);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setTextFormat(Qt::PlainText);
        value->setAccessibleName(name);
        value->setCursor(Qt::PointingHandCursor);
        value->setToolTip(QStringLiteral("单击跳到相关步骤，双击查看全文。"));
        value->installEventFilter(this);
        auto *row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        auto *nameLabel = makeLabel(name, "fieldLabel");
        nameLabel->setMinimumWidth(96);
        nameLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        value->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        rowLayout->addWidget(nameLabel, 0, Qt::AlignTop);
        rowLayout->addWidget(value, 1, Qt::AlignTop);
        conclusionLayout->addWidget(row);
        *slot = value;
    };
    addValue(QStringLiteral("报错信息"), &m_fault, QStringLiteral("faultLabel"));
    addValue(QStringLiteral("所在步骤"), &m_place, QStringLiteral("placeLabel"));
    addValue(QStringLiteral("可能原因"), &m_likely, QStringLiteral("likelyLabel"));
    addValue(QStringLiteral("建议解决方案"), &m_advice, QStringLiteral("adviceLabel"));
    conclusionLayout->addStretch(1);

    auto *detailCard = makeCard();
    auto *detailLayout = new QVBoxLayout(detailCard);
    detailLayout->setContentsMargins(8, 6, 8, 6);
    detailLayout->setSpacing(4);
    auto *detailHeader = new QHBoxLayout;
    detailHeader->setSpacing(6);
    auto *detailTitle = makeLabel(QStringLiteral("本步"), "sectionTitle");
    detailTitle->setObjectName(QStringLiteral("detailTitle"));
    m_copy = new QPushButton(QStringLiteral("复制命令"));
    plainButton(m_copy);
    m_copy->setObjectName(QStringLiteral("copyCommandButton"));
    m_copy->setAccessibleName(QStringLiteral("复制当前步骤的命令"));
    m_copy->setToolTip(QStringLiteral("复制这一步实际使用的命令。还没分析时复制说明里的对应段落。"));
    detailHeader->addWidget(detailTitle);
    detailHeader->addStretch(1);
    detailHeader->addWidget(m_copy);
    detailLayout->addLayout(detailHeader);
    m_detail = new QPlainTextEdit;
    m_detail->setObjectName(QStringLiteral("stepDetailView"));
    m_detail->setReadOnly(true);
    m_detail->setAccessibleName(QStringLiteral("本步详情"));
    m_detail->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_detail->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_detail->setMinimumHeight(48);
    m_detail->setToolTip(QStringLiteral("当前步骤的命令、输出和中文释义。双击左侧步骤可弹出全文。"));
    detailLayout->addWidget(m_detail, 1);

    auto *reportCard = makeCard();
    auto *reportLayout = new QVBoxLayout(reportCard);
    reportLayout->setContentsMargins(8, 6, 8, 6);
    reportLayout->setSpacing(4);
    auto *reportHeader = new QHBoxLayout;
    reportHeader->setSpacing(6);
    auto *commandTitle = makeLabel(QStringLiteral("分析说明"), "sectionTitle");
    commandTitle->setObjectName(QStringLiteral("commandTitle"));
    m_findEdit = new QLineEdit;
    m_findEdit->setObjectName(QStringLiteral("findEdit"));
    m_findEdit->setPlaceholderText(QStringLiteral("在说明中查找"));
    m_findEdit->setAccessibleName(QStringLiteral("在说明中查找"));
    m_findEdit->setClearButtonEnabled(true);
    m_findEdit->setMaximumWidth(220);
    m_findEdit->setToolTip(QStringLiteral("输入文字后回车，在这份说明里定位下一处。"));
    reportHeader->addWidget(commandTitle);
    reportHeader->addStretch(1);
    reportHeader->addWidget(m_findEdit);
    reportLayout->addLayout(reportHeader);
    m_command = new QPlainTextEdit;
    m_command->setObjectName(QStringLiteral("commandView"));
    m_command->setReadOnly(true);
    m_command->setAccessibleName(QStringLiteral("分析说明"));
    m_command->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_command->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_command->setMinimumHeight(72);
    m_command->setPlainText(CoreTriage::primer());
    m_command->setToolTip(QStringLiteral("这次要执行或已经执行的命令、输出和中文释义。导出的就是这一份。"));
    reportLayout->addWidget(m_command, 1);

    m_detailSplit->addWidget(conclusion);
    m_detailSplit->addWidget(detailCard);
    m_columns->addWidget(stepsCard);
    m_columns->addWidget(m_detailSplit);
    m_vertical->addWidget(m_columns);
    m_vertical->addWidget(reportCard);
    m_vertical->setStretchFactor(0, 3);
    m_vertical->setStretchFactor(1, 2);
    m_columns->setStretchFactor(0, 2);
    m_columns->setStretchFactor(1, 5);
    m_detailSplit->setStretchFactor(0, 3);
    m_detailSplit->setStretchFactor(1, 2);
    layout->addWidget(m_vertical, 1);

    setTabOrder(m_coreEdit, coreBrowse);
    setTabOrder(coreBrowse, conditions);
    setTabOrder(conditions, m_run);
    setTabOrder(m_run, m_stop);
    setTabOrder(m_stop, m_save);
    setTabOrder(m_save, m_steps);
    setTabOrder(m_steps, m_detail);
    setTabOrder(m_detail, m_copy);
    setTabOrder(m_copy, m_findEdit);
    setTabOrder(m_findEdit, m_command);

    connect(coreBrowse, &QPushButton::clicked, this, [this]() {
        browseFile(m_coreEdit, QStringLiteral("选择 core 文件"));
    });
    connect(programBrowse, &QPushButton::clicked, this, [this]() {
        browseFile(m_programEdit, QStringLiteral("选择程序文件"));
    });
    connect(debugFile, &QPushButton::clicked, this, [this]() {
        browseFile(m_debugEdit, QStringLiteral("选择调试符号文件"));
    });
    connect(debugDir, &QPushButton::clicked, this, &HighAnalyzePage::browseDebugDirectory);
    connect(conditions, &QPushButton::clicked, this, &HighAnalyzePage::openConditions);
    connect(m_run, &QPushButton::clicked, this, &HighAnalyzePage::start);
    connect(m_coreEdit, &QLineEdit::returnPressed, this, &HighAnalyzePage::start);
    connect(m_stop, &QPushButton::clicked, this, &HighAnalyzePage::cancel);
    connect(m_save, &QPushButton::clicked, this, &HighAnalyzePage::exportReport);
    connect(m_copy, &QPushButton::clicked, this, &HighAnalyzePage::copyCurrentCommand);
    connect(m_findEdit, &QLineEdit::returnPressed, this, &HighAnalyzePage::findInReport);
    connect(m_coreEdit, &QLineEdit::editingFinished, this, &HighAnalyzePage::storePaths);
    connect(m_programEdit, &QLineEdit::editingFinished, this, &HighAnalyzePage::storePaths);
    connect(m_debugEdit, &QLineEdit::editingFinished, this, &HighAnalyzePage::storePaths);
    connect(m_steps, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) { focusStep(row); });
    connect(m_steps, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { openStepDialog(row); });
    connect(m_steps, &QTableWidget::customContextMenuRequested, this, &HighAnalyzePage::onStepMenu);

    auto rememberDrag = [this](int, int) {
        if (!m_adjusting)
            m_userSized = true;
    };
    connect(m_vertical, &QSplitter::splitterMoved, this, rememberDrag);
    connect(m_columns, &QSplitter::splitterMoved, this, rememberDrag);
    connect(m_detailSplit, &QSplitter::splitterMoved, this, rememberDrag);

    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, this, [this]() {
        m_findEdit->setFocus();
        m_findEdit->selectAll();
    });
    auto *cancelShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    cancelShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(cancelShortcut, &QShortcut::activated, this, &HighAnalyzePage::cancel);

    loadPaths();
    fillPreviewSteps();
    refreshControls();
}

HighAnalyzePage::~HighAnalyzePage()
{
    m_closing = true;
    abort();
}

void HighAnalyzePage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    ensurePanes();
}

void HighAnalyzePage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!m_vertical || m_adjusting || m_userSized)
        return;
    ensurePanes();
}

bool HighAnalyzePage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_steps && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            openStepDialog(m_steps->currentRow());
            return true;
        }
    }
    auto *label = qobject_cast<QLabel *>(watched);
    if (label == m_fault || label == m_place || label == m_likely || label == m_advice) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            openValueDialog(label);
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton)
                jumpFromLabel(label);
        }
    }
    return QWidget::eventFilter(watched, event);
}

void HighAnalyzePage::abort()
{
    TriageThread *thread = m_thread.data();
    if (thread) {
        thread->askStop();
        disconnect(thread, nullptr, this, nullptr);
        m_thread = nullptr;
        // 等到 run() 返回再离开。超时后销毁 QThread 会在线程仍在栈上时发生。
        thread->wait();
        if (!m_closing)
            thread->deleteLater();
    }
    m_running = false;
    m_lock.release();
}

bool HighAnalyzePage::saveReport(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = m_command->toPlainText().toUtf8();
    if (file.write(bytes) != bytes.size()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

void HighAnalyzePage::browseFile(QLineEdit *edit, const QString &title)
{
    QWidget *parent = m_conditions && m_conditions->isVisible() ? static_cast<QWidget *>(m_conditions)
                                                                : m_dialogParent.data();
    const QString path = QFileDialog::getOpenFileName(parent, title, startDirectory(edit),
                                                      QStringLiteral("所有文件 (*)"));
    if (path.isEmpty())
        return;
    edit->setText(path);
    storePaths();
}

void HighAnalyzePage::browseDebugDirectory()
{
    QWidget *parent = m_conditions && m_conditions->isVisible() ? static_cast<QWidget *>(m_conditions)
                                                                : m_dialogParent.data();
    const QString path = QFileDialog::getExistingDirectory(parent, QStringLiteral("选择调试符号目录"),
                                                           startDirectory(m_debugEdit));
    if (path.isEmpty())
        return;
    m_debugEdit->setText(path);
    storePaths();
}

QString HighAnalyzePage::dataDirectory() const
{
    QString error;
    return m_services.dataDir(&error);
}

void HighAnalyzePage::loadPaths()
{
    const QString dir = dataDirectory();
    if (dir.isEmpty())
        return;
    QFile file(QDir(dir).filePath(QStringLiteral("paths.txt")));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    if (lines.size() > 0)
        m_coreEdit->setText(lines.at(0));
    if (lines.size() > 1)
        m_programEdit->setText(lines.at(1));
    if (lines.size() > 2)
        m_debugEdit->setText(lines.at(2));
}

void HighAnalyzePage::storePaths() const
{
    const QString dir = dataDirectory();
    if (dir.isEmpty())
        return;
    QFile file(QDir(dir).filePath(QStringLiteral("paths.txt")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    const QString text = m_coreEdit->text() + QLatin1Char('\n') + m_programEdit->text()
        + QLatin1Char('\n') + m_debugEdit->text() + QLatin1Char('\n');
    file.write(text.toUtf8());
}

void HighAnalyzePage::openConditions()
{
    if (m_conditions->exec() == QDialog::Accepted)
        storePaths();
}

void HighAnalyzePage::showSteps(const QVector<TriageStep> &steps, const QStringList &markers)
{
    m_shown = steps;
    m_markers = markers;
    if (m_markers.size() != steps.size()) {
        m_markers.clear();
        for (int i = 0; i < steps.size(); ++i)
            m_markers << QStringLiteral("【%1】%2").arg(i + 1).arg(steps.at(i).heading);
    }
    m_steps->blockSignals(true);
    m_steps->setRowCount(steps.size());
    for (int i = 0; i < steps.size(); ++i) {
        const TriageStep &step = steps.at(i);
        auto *name = new QTableWidgetItem(QStringLiteral("%1  %2").arg(i + 1).arg(step.heading));
        name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        name->setToolTip(step.purpose.isEmpty() ? step.heading
                                                : step.heading + QLatin1Char('\n') + step.purpose);
        const QString situation = step.situation.isEmpty() ? QStringLiteral("未执行") : step.situation;
        auto *state = new QTableWidgetItem(situation);
        state->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        state->setToolTip(situation);
        m_steps->setItem(i, 0, name);
        m_steps->setItem(i, 1, state);
    }
    m_steps->blockSignals(false);
    if (m_steps->rowCount() > 0)
        m_steps->setCurrentCell(0, 0);
    else
        focusStep(-1);
}

void HighAnalyzePage::fillPreviewSteps()
{
    QVector<TriageStep> steps;
    const QStringList headings = {QStringLiteral("辨认文件"), QStringLiteral("读取停止信号"),
                                  QStringLiteral("核对调试信息"), QStringLiteral("查看停在哪一步")};
    const QStringList markers = {QStringLiteral("1. 辨认文件"), QStringLiteral("2. 读取停止信号"),
                                 QStringLiteral("3. 核对调试信息"), QStringLiteral("4. 查看停在哪一步")};
    for (const QString &heading : headings) {
        TriageStep step;
        step.heading = heading;
        step.situation = QStringLiteral("尚未执行");
        step.purpose = QStringLiteral("命令和中文释义在下方说明里。");
        steps.append(step);
    }
    showSteps(steps, markers);
}

void HighAnalyzePage::focusStep(int index)
{
    if (!m_detail)
        return;
    if (index < 0 || index >= m_shown.size()) {
        m_detail->clear();
        refreshControls();
        return;
    }
    const TriageStep &step = m_shown.at(index);
    QString section;
    if (step.commandLine.isEmpty() && index < m_markers.size()) {
        const QString next = index + 1 < m_markers.size() ? m_markers.at(index + 1) : QString();
        section = sliceDocument(m_command->toPlainText(), m_markers.at(index), next);
    }
    m_detail->setPlainText(stepDetailText(step, section));
    if (index < m_markers.size())
        revealInReport(m_markers.at(index));
    refreshControls();
}

void HighAnalyzePage::openStepDialog(int index)
{
    if (index < 0 || index >= m_shown.size())
        return;
    if (m_steps->currentRow() != index)
        m_steps->setCurrentCell(index, 0);
    else
        focusStep(index);
    showTextDialog(m_shown.at(index).heading, m_detail->toPlainText());
}

void HighAnalyzePage::openValueDialog(QLabel *label)
{
    if (!label)
        return;
    showTextDialog(label->accessibleName(), label->text());
}

void HighAnalyzePage::jumpFromLabel(QLabel *label)
{
    if (label == m_advice) {
        revealInReport(QStringLiteral("建议解决方案"));
        return;
    }
    int index = m_shown.isEmpty() ? -1 : m_shown.size() - 1;
    if (label == m_fault)
        index = qMin(1, index);
    else if (label == m_likely)
        index = qMax(0, index);
    if (index < 0)
        return;
    m_steps->setCurrentCell(index, 0);
    focusStep(index);
}

void HighAnalyzePage::revealInReport(const QString &marker)
{
    if (!m_command || marker.isEmpty())
        return;
    const QTextCursor found = m_command->document()->find(marker);
    if (found.isNull())
        return;
    m_command->setTextCursor(found);
    m_command->ensureCursorVisible();
}

void HighAnalyzePage::findInReport()
{
    const QString key = m_findEdit->text();
    if (key.isEmpty())
        return;
    QTextDocument *document = m_command->document();
    QTextCursor found = document->find(key, m_command->textCursor());
    if (found.isNull())
        found = document->find(key);
    if (found.isNull()) {
        m_services.showStatus(StatusState::Info, QStringLiteral("说明里没有“%1”").arg(key));
        return;
    }
    m_command->setTextCursor(found);
    m_command->ensureCursorVisible();
    m_command->setFocus();
}

void HighAnalyzePage::copyCurrentCommand()
{
    const int index = m_steps ? m_steps->currentRow() : -1;
    QString text;
    if (index >= 0 && index < m_shown.size() && !m_shown.at(index).commandLine.isEmpty())
        text = m_shown.at(index).commandLine;
    else if (m_detail)
        text = m_detail->toPlainText();
    if (text.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
    m_services.showStatus(StatusState::Info, QStringLiteral("已复制"));
}

void HighAnalyzePage::showTextDialog(const QString &title, const QString &body)
{
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("textDialog"));
    dialog->setWindowTitle(title.isEmpty() ? QStringLiteral("全文") : title);
    dialog->resize(560, 420);
    auto *layout = new QVBoxLayout(dialog);
    auto *view = new QPlainTextEdit;
    view->setObjectName(QStringLiteral("textDialogBody"));
    view->setReadOnly(true);
    view->setPlainText(body);
    view->setAccessibleName(title);
    layout->addWidget(view);
    auto *buttons = new QHBoxLayout;
    auto *copy = new QPushButton(QStringLiteral("复制"));
    plainButton(copy);
    copy->setObjectName(QStringLiteral("textDialogCopy"));
    auto *close = new QPushButton(QStringLiteral("关闭"));
    plainButton(close);
    close->setObjectName(QStringLiteral("textDialogClose"));
    buttons->addStretch(1);
    buttons->addWidget(copy);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(copy, &QPushButton::clicked, dialog, [view]() {
        QGuiApplication::clipboard()->setText(view->toPlainText());
    });
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    dialog->show();
}

void HighAnalyzePage::setProgressText(const QString &text)
{
    m_progressFull = text;
    const QString shown = m_progress->fontMetrics().elidedText(text, Qt::ElideRight, 180);
    m_progress->setText(shown);
    m_progress->setToolTip(text);
}

void HighAnalyzePage::ensurePanes()
{
    if (!m_vertical || !m_columns || !m_detailSplit || m_userSized)
        return;
    if (!isVisible() && m_vertical->height() < 40)
        return;
    m_adjusting = true;

    const int colW = m_columns->width() > 0 ? m_columns->width() : qMax(0, width() - 16);
    const int stepsW = qBound(200, colW * 30 / 100, 320);
    if (colW > stepsW + 80)
        m_columns->setSizes({stepsW, colW - stepsW});

    const int rightW = qMax(0, colW - stepsW);
    const int conclusionW = rightW * 66 / 100;
    if (rightW > 80)
        m_detailSplit->setSizes({conclusionW, rightW - conclusionW});

    int textWidth = conclusionW - 128;
    if (textWidth < 100)
        textWidth = qMax(100, m_fault ? m_fault->width() : 100);

    int content = 28;
    QLabel *values[] = {m_fault, m_place, m_likely, m_advice};
    for (QLabel *label : values) {
        if (!label)
            continue;
        const int row = qMax(label->fontMetrics().lineSpacing() + 6, label->heightForWidth(textWidth));
        label->setMinimumHeight(row);
        content += row + 6;
    }
    content += 20;

    const int total = m_vertical->height() > 0 ? m_vertical->height() : qMax(200, height() - 40);
    const int handle = m_vertical->handleWidth();
    const int bottomMin = 120;
    int top = qMax(content, total * 58 / 100);
    int bottom = total - handle - top;
    if (bottom < bottomMin && total > bottomMin + handle + 80) {
        bottom = bottomMin;
        top = total - handle - bottom;
    }
    if (top < 80)
        top = qMax(80, total - handle - bottomMin);
    bottom = qMax(60, total - handle - top);
    m_vertical->setSizes({top, bottom});
    m_adjusting = false;
}

void HighAnalyzePage::onStepMenu(const QPoint &pos)
{
    auto *item = m_steps->itemAt(pos);
    if (!item)
        return;
    m_steps->setCurrentCell(item->row(), 0);
    QMenu menu(this);
    menu.addAction(QStringLiteral("在说明中定位"), this, [this, item]() { focusStep(item->row()); });
    menu.addAction(QStringLiteral("查看全文"), this, [this, item]() { openStepDialog(item->row()); });
    menu.addAction(QStringLiteral("复制命令"), this, &HighAnalyzePage::copyCurrentCommand);
    menu.exec(m_steps->viewport()->mapToGlobal(pos));
}

void HighAnalyzePage::start()
{
    if (m_running)
        return;
    storePaths();
    const QString core = m_coreEdit->text().trimmed();
    if (core.isEmpty()) {
        m_fault->setText(QStringLiteral("请先选择一个 core 文件。"));
        m_services.showStatus(StatusState::Error, QStringLiteral("请先选择 core 文件"));
        m_services.logWarning(QStringLiteral("未选择 core 文件"));
        ensurePanes();
        return;
    }
    if (m_thread)
        abort();

    m_running = true;
    m_lock = m_services.lockNavigation(QStringLiteral("正在分析 core 文件"));
    m_services.logInfo(QStringLiteral("开始分析 %1").arg(QFileInfo(core).fileName()));
    m_services.showStatus(StatusState::Busy, QStringLiteral("正在分析 core 文件"));
    setProgressText(QStringLiteral("正在准备"));
    refreshControls();

    TriageInput input;
    input.corePath = core;
    input.programPath = m_programEdit->text().trimmed();
    input.debugPath = m_debugEdit->text().trimmed();
    input.directory = dataDirectory();
    auto *thread = new TriageThread(input, m_locator, m_factory, this);
    m_thread = thread;
    connect(thread, &TriageThread::note, this, &HighAnalyzePage::onNote);
    connect(thread, &QThread::finished, this, &HighAnalyzePage::onFinished);
    thread->start();
}

void HighAnalyzePage::cancel()
{
    if (!m_thread)
        return;
    m_thread->askStop();
    m_stop->setEnabled(false);
    setProgressText(QStringLiteral("正在取消"));
    m_services.showStatus(StatusState::Busy, QStringLiteral("正在取消分析"));
}

void HighAnalyzePage::exportReport()
{
    const QString dir = dataDirectory();
    const QString suggested = dir.isEmpty()
        ? QStringLiteral("文件分析high说明.txt")
        : QDir(dir).filePath(QStringLiteral("文件分析high说明.txt"));
    const QString path = QFileDialog::getSaveFileName(m_dialogParent, QStringLiteral("导出分析说明"),
                                                      suggested, QStringLiteral("文本 (*.txt)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!saveReport(path, &error)) {
        m_services.showStatus(StatusState::Error, QStringLiteral("导出失败"));
        m_services.logError(QStringLiteral("导出失败：%1").arg(error));
        return;
    }
    m_services.showStatus(StatusState::Success, QStringLiteral("说明已导出"));
    m_services.logSuccess(QStringLiteral("已导出说明"));
}

void HighAnalyzePage::onNote(const QString &text)
{
    if (!m_running)
        return;
    setProgressText(text);
    m_services.showStatus(StatusState::Busy, text);
}

void HighAnalyzePage::onFinished()
{
    auto *thread = qobject_cast<TriageThread *>(sender());
    if (!thread || thread != m_thread)
        return;
    const TriageOutcome outcome = thread->outcome();
    m_thread = nullptr;
    thread->deleteLater();
    m_running = false;
    m_lock.release();
    showOutcome(outcome);
    refreshControls();
}

void HighAnalyzePage::showOutcome(const TriageOutcome &outcome)
{
    m_fault->setText(outcome.fault.isEmpty() ? QStringLiteral("无") : outcome.fault);
    m_place->setText(outcome.place.isEmpty() ? QStringLiteral("无") : outcome.place);
    m_likely->setText(outcome.likely.isEmpty() ? QStringLiteral("无") : outcome.likely);
    m_advice->setText(outcome.advice.isEmpty() ? QStringLiteral("无") : outcome.advice);
    if (!outcome.document.isEmpty())
        m_command->setPlainText(outcome.document);
    if (!outcome.steps.isEmpty())
        showSteps(outcome.steps, {});

    const QString dir = dataDirectory();
    if (!dir.isEmpty() && !outcome.document.isEmpty()) {
        QFile out(QDir(dir).filePath(QStringLiteral("brief.txt")));
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            out.write(outcome.document.toUtf8());
    }
    ensurePanes();

    if (outcome.stoppedEarly) {
        setProgressText(QStringLiteral("已取消"));
        m_services.showStatus(StatusState::Info, QStringLiteral("已取消分析"));
        m_services.logWarning(QStringLiteral("分析已取消"));
        return;
    }
    if (outcome.accepted) {
        setProgressText(QStringLiteral("分析完成"));
        const QString brief = outcome.signalToken.isEmpty()
            ? QStringLiteral("分析完成")
            : QStringLiteral("分析完成：%1").arg(outcome.signalToken);
        m_services.showStatus(StatusState::Success, brief);
        m_services.logSuccess(brief);
        return;
    }
    setProgressText(QStringLiteral("分析结束"));
    m_services.showStatus(StatusState::Error, QStringLiteral("分析失败"));
    m_services.logError(outcome.fault);
}

void HighAnalyzePage::refreshControls()
{
    const auto buttons = findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        const QString name = button->objectName();
        if (name == QLatin1String("textDialogClose") || name == QLatin1String("textDialogCopy"))
            button->setEnabled(true);
        else if (button == m_stop)
            button->setEnabled(m_running);
        else if (button == m_save)
            button->setEnabled(!m_running && m_command && !m_command->toPlainText().isEmpty());
        else if (button == m_copy)
            button->setEnabled(m_detail && !m_detail->toPlainText().isEmpty());
        else
            button->setEnabled(!m_running);
    }
    m_coreEdit->setEnabled(!m_running);
    m_programEdit->setEnabled(!m_running);
    m_debugEdit->setEnabled(!m_running);
    if (m_findEdit)
        m_findEdit->setEnabled(true);
    if (m_running) {
        m_bar->setRange(0, 0);
    } else {
        m_bar->setRange(0, 1);
        m_bar->setValue(m_progressFull == QStringLiteral("分析完成") ? 1 : 0);
    }
}

ToolInfo HighAnalyzeTool::info() const
{
    ToolInfo info;
    info.id = QStringLiteral("core-analyze-high");
    info.title = QStringLiteral("文件分析high");
    info.summary = QStringLiteral("查看 Linux core 文件为何停止，并写出能看懂的说明");
    info.iconKey = QStringLiteral("status");
    info.order = 40;
    return info;
}

bool HighAnalyzeTool::initialize(const ToolContext &context, QString *error)
{
    m_context = context;
    QString dirError;
    if (m_context.services.dataDir(&dirError).isEmpty()) {
        if (error)
            *error = dirError.isEmpty() ? QStringLiteral("数据目录不可用") : dirError;
        return false;
    }
    return true;
}

QVector<ToolPage> HighAnalyzeTool::createPages()
{
    auto *page = new HighAnalyzePage(m_context.services, m_context.dialogParent);
    m_page = page;
    QVector<ToolPage> pages;
    pages.append({PageInfo{QStringLiteral("read"), QStringLiteral("分析"),
                           QStringLiteral("查看 core 文件并导出说明"), QStringLiteral("status")},
                  page});
    return pages;
}

void HighAnalyzeTool::shutdown()
{
    if (m_page)
        m_page->abort();
}

} // namespace tw
