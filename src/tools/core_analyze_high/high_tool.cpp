#include "tools/core_analyze_high/high_tool.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStringList>
#include <QVBoxLayout>
#include <QVariant>

namespace tw {
namespace {

// 换行标签的 minimumSizeHint 在宽度还没确定时会非常高，把结论和导出按钮顶出窗口。
// 这里把最小高度收成一行，真正高度仍由布局按宽度计算。
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
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);

    layout->addWidget(makeLabel(QStringLiteral("文件分析high"), "pageTitle"));
    layout->addWidget(makeLabel(
        QStringLiteral("选择 Linux core 文件，查看程序为何停止，并生成一份能看懂的说明。"),
        "pageSubtitle"));

    auto *pick = new QFrame;
    pick->setProperty("role", QStringLiteral("card"));
    pick->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    auto *pickLayout = new QVBoxLayout(pick);
    pickLayout->setContentsMargins(12, 10, 12, 10);
    pickLayout->setSpacing(6);

    m_coreEdit = new QLineEdit;
    m_coreEdit->setObjectName(QStringLiteral("coreFileEdit"));
    m_coreEdit->setPlaceholderText(QStringLiteral("选择 core 文件"));
    m_coreEdit->setAccessibleName(QStringLiteral("core 文件"));
    m_coreEdit->setClearButtonEnabled(true);
    auto *coreBrowse = new QPushButton(QStringLiteral("浏览"));
    coreBrowse->setAccessibleName(QStringLiteral("浏览 core 文件"));

    m_programEdit = new QLineEdit;
    m_programEdit->setObjectName(QStringLiteral("programFileEdit"));
    m_programEdit->setPlaceholderText(QStringLiteral("可选"));
    m_programEdit->setAccessibleName(QStringLiteral("程序文件"));
    m_programEdit->setClearButtonEnabled(true);
    auto *programBrowse = new QPushButton(QStringLiteral("浏览"));
    programBrowse->setAccessibleName(QStringLiteral("浏览程序文件"));

    m_debugEdit = new QLineEdit;
    m_debugEdit->setObjectName(QStringLiteral("debugPathEdit"));
    m_debugEdit->setPlaceholderText(QStringLiteral("可选，文件或目录"));
    m_debugEdit->setAccessibleName(QStringLiteral("调试符号"));
    m_debugEdit->setClearButtonEnabled(true);
    auto *debugFile = new QPushButton(QStringLiteral("文件"));
    debugFile->setAccessibleName(QStringLiteral("浏览调试符号文件"));
    auto *debugDir = new QPushButton(QStringLiteral("目录"));
    debugDir->setAccessibleName(QStringLiteral("浏览调试符号目录"));

    pickLayout->addWidget(pathRow(makeLabel(QStringLiteral("core 文件"), "fieldLabel"), m_coreEdit,
                                  coreBrowse));
    pickLayout->addWidget(pathRow(makeLabel(QStringLiteral("程序文件"), "fieldLabel"), m_programEdit,
                                  programBrowse));
    pickLayout->addWidget(pathRow(makeLabel(QStringLiteral("调试符号"), "fieldLabel"), m_debugEdit,
                                  debugFile, debugDir));
    pickLayout->addWidget(makeLabel(
        QStringLiteral("调试符号用来把地址翻译成步骤名。没有也可以分析。"), "hint"));

    m_bar = new QProgressBar;
    m_bar->setObjectName(QStringLiteral("progressBar"));
    m_bar->setAccessibleName(QStringLiteral("分析进度"));
    m_bar->setRange(0, 1);
    m_bar->setValue(0);
    m_bar->setTextVisible(false);
    m_progress = makeLabel(QStringLiteral("尚未分析"), "hint");
    m_progress->setObjectName(QStringLiteral("progressLabel"));

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    m_run = new QPushButton(QStringLiteral("开始分析"));
    m_run->setObjectName(QStringLiteral("runButton"));
    m_run->setProperty("role", QStringLiteral("primary"));
    m_run->setAccessibleName(QStringLiteral("开始分析"));
    m_run->setDefault(true);
    m_stop = new QPushButton(QStringLiteral("取消"));
    m_stop->setObjectName(QStringLiteral("stopButton"));
    m_stop->setAccessibleName(QStringLiteral("取消分析"));
    m_save = new QPushButton(QStringLiteral("导出说明"));
    m_save->setObjectName(QStringLiteral("saveButton"));
    m_save->setAccessibleName(QStringLiteral("导出分析说明"));
    buttons->addWidget(m_run);
    buttons->addWidget(m_stop);
    buttons->addWidget(m_save);
    buttons->addStretch(1);

    pickLayout->addWidget(m_bar);
    pickLayout->addWidget(m_progress);
    pickLayout->addLayout(buttons);
    layout->addWidget(pick);

    auto *body = new QWidget;
    body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(12);

    auto *conclusion = new QFrame;
    conclusion->setProperty("role", QStringLiteral("card"));
    conclusion->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *conclusionLayout = new QVBoxLayout(conclusion);
    conclusionLayout->setContentsMargins(12, 10, 12, 10);
    conclusionLayout->setSpacing(6);
    auto *conclusionTitle = makeLabel(QStringLiteral("结论"), "sectionTitle");
    conclusionTitle->setObjectName(QStringLiteral("conclusionTitle"));
    conclusionLayout->addWidget(conclusionTitle);

    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("conclusionScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    scroll->setMinimumHeight(110);
    auto *fields = new QWidget;
    auto *grid = new QGridLayout(fields);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(1, 1);

    auto addValue = [&](int row, const QString &name, QLabel **slot, const QString &objectName) {
        grid->addWidget(makeLabel(name, "fieldLabel"), row, 0, Qt::AlignTop);
        auto *value = makeLabel(QStringLiteral("尚未分析"), "value");
        value->setObjectName(objectName);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setTextFormat(Qt::PlainText);
        value->setAccessibleName(name);
        grid->addWidget(value, row, 1);
        *slot = value;
    };
    addValue(0, QStringLiteral("报错信息"), &m_fault, QStringLiteral("faultLabel"));
    addValue(1, QStringLiteral("所在步骤"), &m_place, QStringLiteral("placeLabel"));
    addValue(2, QStringLiteral("可能原因"), &m_likely, QStringLiteral("likelyLabel"));
    addValue(3, QStringLiteral("建议解决方案"), &m_advice, QStringLiteral("adviceLabel"));
    scroll->setWidget(fields);
    conclusionLayout->addWidget(scroll, 1);

    auto *commands = new QWidget;
    commands->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *commandLayout = new QVBoxLayout(commands);
    commandLayout->setContentsMargins(0, 0, 0, 0);
    commandLayout->setSpacing(6);
    auto *commandTitle = makeLabel(QStringLiteral("命令说明"), "sectionTitle");
    commandTitle->setObjectName(QStringLiteral("commandTitle"));
    commandLayout->addWidget(commandTitle);
    commandLayout->addWidget(makeLabel(
        QStringLiteral("分析完成后，这里换成这次真正执行的命令、输出和中文释义。"), "hint"));
    m_command = new QPlainTextEdit;
    m_command->setObjectName(QStringLiteral("commandView"));
    m_command->setReadOnly(true);
    m_command->setAccessibleName(QStringLiteral("命令说明"));
    m_command->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_command->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_command->setMinimumHeight(140);
    m_command->setPlainText(CoreTriage::primer());
    commandLayout->addWidget(m_command, 1);

    bodyLayout->addWidget(conclusion, 4);
    bodyLayout->addWidget(commands, 6);
    layout->addWidget(body, 1);

    setTabOrder(m_coreEdit, coreBrowse);
    setTabOrder(coreBrowse, m_programEdit);
    setTabOrder(m_programEdit, programBrowse);
    setTabOrder(programBrowse, m_debugEdit);
    setTabOrder(m_debugEdit, debugFile);
    setTabOrder(debugFile, debugDir);
    setTabOrder(debugDir, m_run);
    setTabOrder(m_run, m_stop);
    setTabOrder(m_stop, m_save);
    setTabOrder(m_save, m_command);

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
    connect(m_run, &QPushButton::clicked, this, &HighAnalyzePage::start);
    connect(m_stop, &QPushButton::clicked, this, &HighAnalyzePage::cancel);
    connect(m_save, &QPushButton::clicked, this, &HighAnalyzePage::exportReport);
    connect(m_coreEdit, &QLineEdit::editingFinished, this, &HighAnalyzePage::storePaths);
    connect(m_programEdit, &QLineEdit::editingFinished, this, &HighAnalyzePage::storePaths);
    connect(m_debugEdit, &QLineEdit::editingFinished, this, &HighAnalyzePage::storePaths);

    loadPaths();
    refreshControls();
}

HighAnalyzePage::~HighAnalyzePage()
{
    m_closing = true;
    abort();
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
    const QString path = QFileDialog::getOpenFileName(m_dialogParent, title, startDirectory(edit),
                                                      QStringLiteral("所有文件 (*)"));
    if (path.isEmpty())
        return;
    edit->setText(path);
    storePaths();
}

void HighAnalyzePage::browseDebugDirectory()
{
    const QString path = QFileDialog::getExistingDirectory(m_dialogParent,
                                                           QStringLiteral("选择调试符号目录"),
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
        return;
    }
    if (m_thread)
        abort();

    m_running = true;
    m_lock = m_services.lockNavigation(QStringLiteral("正在分析 core 文件"));
    m_services.logInfo(QStringLiteral("开始分析 %1").arg(QFileInfo(core).fileName()));
    m_services.showStatus(StatusState::Busy, QStringLiteral("正在分析 core 文件"));
    m_progress->setText(QStringLiteral("正在准备"));
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
    m_progress->setText(QStringLiteral("正在取消"));
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
    m_progress->setText(text);
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

    const QString dir = dataDirectory();
    if (!dir.isEmpty() && !outcome.document.isEmpty()) {
        QFile out(QDir(dir).filePath(QStringLiteral("brief.txt")));
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            out.write(outcome.document.toUtf8());
    }

    if (outcome.stoppedEarly) {
        m_progress->setText(QStringLiteral("已取消"));
        m_services.showStatus(StatusState::Info, QStringLiteral("已取消分析"));
        m_services.logWarning(QStringLiteral("分析已取消"));
        return;
    }
    if (outcome.accepted) {
        m_progress->setText(QStringLiteral("分析完成"));
        const QString brief = outcome.signalToken.isEmpty()
            ? QStringLiteral("分析完成")
            : QStringLiteral("分析完成：%1").arg(outcome.signalToken);
        m_services.showStatus(StatusState::Success, brief);
        m_services.logSuccess(brief);
        return;
    }
    m_progress->setText(QStringLiteral("分析结束"));
    m_services.showStatus(StatusState::Error, QStringLiteral("分析失败"));
    m_services.logError(outcome.fault);
}

void HighAnalyzePage::refreshControls()
{
    const auto buttons = findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button == m_stop)
            button->setEnabled(m_running);
        else if (button == m_save)
            button->setEnabled(!m_running && m_command && !m_command->toPlainText().isEmpty());
        else
            button->setEnabled(!m_running);
    }
    m_coreEdit->setEnabled(!m_running);
    m_programEdit->setEnabled(!m_running);
    m_debugEdit->setEnabled(!m_running);
    if (m_running) {
        m_bar->setRange(0, 0);
    } else {
        m_bar->setRange(0, 1);
        m_bar->setValue(m_progress->text() == QStringLiteral("分析完成") ? 1 : 0);
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
