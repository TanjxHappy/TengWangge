#include "tools/core_analyze/core_analyze_tool.h"

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
#include <QTextStream>
#include <QVBoxLayout>
#include <QVariant>

namespace tw {
namespace {

QLabel *makeLabel(const QString &text, const char *role)
{
    auto *label = new QLabel(text);
    label->setProperty("role", QString::fromLatin1(role));
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    return label;
}

QWidget *pathRow(QLineEdit *edit, QPushButton *browse, QPushButton *extra = nullptr)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    edit->setMinimumWidth(0);
    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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

void AnalyzeThread::run()
{
    std::unique_ptr<CommandExecutor> executor;
    if (m_factory)
        executor = m_factory(&m_cancel);
    else
        executor = std::make_unique<ProcessExecutor>(&m_cancel);
    m_report = CoreAnalyzer::analyze(m_request, *executor, m_finder, &m_cancel,
                                     [this](const QString &text) { emit progressText(text); });
}

CoreAnalyzePage::CoreAnalyzePage(const ToolServices &services, QWidget *dialogParent, QWidget *parent)
    : QWidget(parent)
    , m_services(services)
    , m_dialogParent(dialogParent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setFocusPolicy(Qt::NoFocus);
    scroll->viewport()->setAutoFillBackground(false);
    outer->addWidget(scroll);

    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);
    scroll->setWidget(content);

    layout->addWidget(makeLabel(QStringLiteral("文件分析"), "pageTitle"));
    layout->addWidget(makeLabel(
        QStringLiteral("选择一份 Linux core 文件，查看程序为何停止，并生成一份能看懂的说明。"),
        "pageSubtitle"));

    auto *pick = new QFrame;
    pick->setProperty("role", QStringLiteral("card"));
    auto *pickLayout = new QVBoxLayout(pick);
    pickLayout->setContentsMargins(16, 14, 16, 14);
    pickLayout->setSpacing(10);

    m_coreEdit = new QLineEdit;
    m_coreEdit->setObjectName(QStringLiteral("corePath"));
    m_coreEdit->setPlaceholderText(QStringLiteral("选择 core 文件"));
    m_coreEdit->setAccessibleName(QStringLiteral("core 文件"));
    m_coreEdit->setClearButtonEnabled(true);
    auto *coreBrowse = new QPushButton(QStringLiteral("浏览"));
    coreBrowse->setAccessibleName(QStringLiteral("浏览 core 文件"));

    m_exeEdit = new QLineEdit;
    m_exeEdit->setObjectName(QStringLiteral("exePath"));
    m_exeEdit->setPlaceholderText(QStringLiteral("可以不填"));
    m_exeEdit->setAccessibleName(QStringLiteral("程序文件"));
    m_exeEdit->setClearButtonEnabled(true);
    auto *exeBrowse = new QPushButton(QStringLiteral("浏览"));
    exeBrowse->setAccessibleName(QStringLiteral("浏览程序文件"));

    m_symbolEdit = new QLineEdit;
    m_symbolEdit->setObjectName(QStringLiteral("symbolPath"));
    m_symbolEdit->setPlaceholderText(QStringLiteral("可以不填，文件或目录都可以"));
    m_symbolEdit->setAccessibleName(QStringLiteral("调试符号"));
    m_symbolEdit->setClearButtonEnabled(true);
    auto *symbolBrowse = new QPushButton(QStringLiteral("文件"));
    symbolBrowse->setAccessibleName(QStringLiteral("浏览调试符号文件"));
    auto *symbolDir = new QPushButton(QStringLiteral("目录"));
    symbolDir->setAccessibleName(QStringLiteral("浏览调试符号目录"));

    pickLayout->addWidget(makeLabel(QStringLiteral("core 文件"), "fieldLabel"));
    pickLayout->addWidget(pathRow(m_coreEdit, coreBrowse));
    pickLayout->addWidget(makeLabel(QStringLiteral("程序文件（可选）"), "fieldLabel"));
    pickLayout->addWidget(pathRow(m_exeEdit, exeBrowse));
    pickLayout->addWidget(makeLabel(QStringLiteral("调试符号（可选）"), "fieldLabel"));
    pickLayout->addWidget(pathRow(m_symbolEdit, symbolBrowse, symbolDir));
    pickLayout->addWidget(makeLabel(
        QStringLiteral("调试符号是一份对照表，用来把地址翻译成步骤名称。没有也可以分析。"),
        "hint"));

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
    m_start = new QPushButton(QStringLiteral("开始分析"));
    m_start->setObjectName(QStringLiteral("startButton"));
    m_start->setProperty("role", QStringLiteral("primary"));
    m_start->setAccessibleName(QStringLiteral("开始分析"));
    m_start->setDefault(true);
    m_cancel = new QPushButton(QStringLiteral("取消"));
    m_cancel->setObjectName(QStringLiteral("cancelButton"));
    m_cancel->setAccessibleName(QStringLiteral("取消分析"));
    m_export = new QPushButton(QStringLiteral("导出说明"));
    m_export->setObjectName(QStringLiteral("exportButton"));
    m_export->setAccessibleName(QStringLiteral("导出分析说明"));
    buttons->addWidget(m_start);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_export);
    buttons->addStretch(1);

    pickLayout->addWidget(m_bar);
    pickLayout->addWidget(m_progress);
    pickLayout->addLayout(buttons);
    layout->addWidget(pick);

    auto *result = new QFrame;
    result->setProperty("role", QStringLiteral("card"));
    auto *grid = new QGridLayout(result);
    grid->setContentsMargins(16, 14, 16, 14);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(10);
    grid->setColumnStretch(1, 1);
    grid->addWidget(makeLabel(QStringLiteral("结论"), "sectionTitle"), 0, 0, 1, 2);

    auto addValue = [&](int row, const QString &name, QLabel **slot, const QString &objectName) {
        grid->addWidget(makeLabel(name, "fieldLabel"), row, 0, Qt::AlignTop);
        auto *value = makeLabel(QStringLiteral("尚未分析"), "value");
        value->setObjectName(objectName);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setAccessibleName(name);
        grid->addWidget(value, row, 1);
        *slot = value;
    };
    addValue(1, QStringLiteral("报错信息"), &m_error, QStringLiteral("errorValue"));
    addValue(2, QStringLiteral("所在步骤"), &m_where, QStringLiteral("whereValue"));
    addValue(3, QStringLiteral("可能原因"), &m_cause, QStringLiteral("causeValue"));
    addValue(4, QStringLiteral("建议解决方案"), &m_suggestion, QStringLiteral("suggestionValue"));
    layout->addWidget(result);

    layout->addWidget(makeLabel(QStringLiteral("分析说明"), "sectionTitle"));
    layout->addWidget(makeLabel(
        QStringLiteral("下面是查看 core 文件的步骤。分析之后，会换成这次真正执行过的命令和输出释义。"),
        "hint"));
    m_report = new QPlainTextEdit;
    m_report->setObjectName(QStringLiteral("reportView"));
    m_report->setReadOnly(true);
    m_report->setAccessibleName(QStringLiteral("分析说明"));
    m_report->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_report->setMinimumHeight(220);
    m_report->setPlainText(CoreAnalyzer::usageGuide());
    layout->addWidget(m_report);

    setTabOrder(m_coreEdit, coreBrowse);
    setTabOrder(coreBrowse, m_exeEdit);
    setTabOrder(m_exeEdit, exeBrowse);
    setTabOrder(exeBrowse, m_symbolEdit);
    setTabOrder(m_symbolEdit, symbolBrowse);
    setTabOrder(symbolBrowse, symbolDir);
    setTabOrder(symbolDir, m_start);
    setTabOrder(m_start, m_cancel);
    setTabOrder(m_cancel, m_export);
    setTabOrder(m_export, m_report);

    connect(coreBrowse, &QPushButton::clicked, this, [this]() {
        browseFile(m_coreEdit, QStringLiteral("选择 core 文件"));
    });
    connect(exeBrowse, &QPushButton::clicked, this, [this]() {
        browseFile(m_exeEdit, QStringLiteral("选择程序文件"));
    });
    connect(symbolBrowse, &QPushButton::clicked, this, [this]() {
        browseFile(m_symbolEdit, QStringLiteral("选择调试符号文件"));
    });
    connect(symbolDir, &QPushButton::clicked, this, &CoreAnalyzePage::browseSymbolDir);
    connect(m_start, &QPushButton::clicked, this, &CoreAnalyzePage::start);
    connect(m_cancel, &QPushButton::clicked, this, &CoreAnalyzePage::cancel);
    connect(m_export, &QPushButton::clicked, this, &CoreAnalyzePage::exportReport);
    connect(m_coreEdit, &QLineEdit::editingFinished, this, &CoreAnalyzePage::saveInputs);
    connect(m_exeEdit, &QLineEdit::editingFinished, this, &CoreAnalyzePage::saveInputs);
    connect(m_symbolEdit, &QLineEdit::editingFinished, this, &CoreAnalyzePage::saveInputs);

    loadInputs();
    updateUi();
}

CoreAnalyzePage::~CoreAnalyzePage()
{
    abort();
}

void CoreAnalyzePage::abort()
{
    if (m_thread) {
        m_thread->requestCancel();
        disconnect(m_thread, nullptr, this, nullptr);
        AnalyzeThread *thread = m_thread;
        m_thread = nullptr;
        if (!thread->wait(8000))
            connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        else
            thread->deleteLater();
    }
    m_running = false;
    m_lock.release();
}

bool CoreAnalyzePage::saveReport(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = m_report->toPlainText().toUtf8();
    if (file.write(bytes) != bytes.size()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

void CoreAnalyzePage::browseFile(QLineEdit *edit, const QString &title)
{
    const QString path = QFileDialog::getOpenFileName(m_dialogParent, title, startDirectory(edit),
                                                      QStringLiteral("所有文件 (*)"));
    if (path.isEmpty())
        return;
    edit->setText(path);
    saveInputs();
}

void CoreAnalyzePage::browseSymbolDir()
{
    const QString path = QFileDialog::getExistingDirectory(m_dialogParent,
                                                           QStringLiteral("选择调试符号目录"),
                                                           startDirectory(m_symbolEdit));
    if (path.isEmpty())
        return;
    m_symbolEdit->setText(path);
    saveInputs();
}

QString CoreAnalyzePage::dataDirectory() const
{
    QString error;
    return m_services.dataDir(&error);
}

void CoreAnalyzePage::loadInputs()
{
    const QString dir = dataDirectory();
    if (dir.isEmpty())
        return;
    QFile file(QDir(dir).filePath(QStringLiteral("last-inputs.txt")));
    if (!file.open(QIODevice::ReadOnly))
        return;
    QTextStream in(&file);
    in.setCodec("UTF-8");
    const QString core = in.readLine();
    const QString exe = in.readLine();
    const QString symbol = in.readLine();
    if (!core.isNull())
        m_coreEdit->setText(core);
    if (!exe.isNull())
        m_exeEdit->setText(exe);
    if (!symbol.isNull())
        m_symbolEdit->setText(symbol);
}

void CoreAnalyzePage::saveInputs() const
{
    const QString dir = dataDirectory();
    if (dir.isEmpty())
        return;
    QFile file(QDir(dir).filePath(QStringLiteral("last-inputs.txt")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    const QString text = m_coreEdit->text() + QLatin1Char('\n')
        + m_exeEdit->text() + QLatin1Char('\n')
        + m_symbolEdit->text() + QLatin1Char('\n');
    file.write(text.toUtf8());
}

void CoreAnalyzePage::start()
{
    if (m_running)
        return;
    saveInputs();
    const QString core = m_coreEdit->text().trimmed();
    if (core.isEmpty()) {
        m_error->setText(QStringLiteral("请先选择一个 core 文件。"));
        m_services.showStatus(StatusState::Error, QStringLiteral("请先选择 core 文件"));
        m_services.logWarning(QStringLiteral("未选择 core 文件"));
        return;
    }

    m_running = true;
    m_lock = m_services.lockNavigation(QStringLiteral("正在分析 core 文件"));
    m_services.logInfo(QStringLiteral("开始分析 %1").arg(QFileInfo(core).fileName()));
    m_services.showStatus(StatusState::Busy, QStringLiteral("正在分析 core 文件"));
    m_progress->setText(QStringLiteral("正在准备"));
    updateUi();

    auto *thread = new AnalyzeThread;
    thread->setFactory(m_factory);
    thread->setFinder(m_finder);
    AnalysisRequest request;
    request.corePath = core;
    request.executablePath = m_exeEdit->text().trimmed();
    request.symbolPath = m_symbolEdit->text().trimmed();
    request.workDirectory = dataDirectory();
    thread->setRequest(request);
    m_thread = thread;
    connect(thread, &AnalyzeThread::progressText, this, &CoreAnalyzePage::onProgress, Qt::QueuedConnection);
    connect(thread, &QThread::finished, this, &CoreAnalyzePage::onFinished, Qt::QueuedConnection);
    thread->start();
}

void CoreAnalyzePage::cancel()
{
    if (!m_thread)
        return;
    m_thread->requestCancel();
    m_cancel->setEnabled(false);
    m_progress->setText(QStringLiteral("正在取消"));
    m_services.showStatus(StatusState::Busy, QStringLiteral("正在取消分析"));
}

void CoreAnalyzePage::exportReport()
{
    const QString dir = dataDirectory();
    const QString suggested = dir.isEmpty()
        ? QStringLiteral("文件分析说明.txt")
        : QDir(dir).filePath(QStringLiteral("文件分析说明.txt"));
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

void CoreAnalyzePage::onProgress(const QString &text)
{
    if (!m_running)
        return;
    m_progress->setText(text);
    m_services.showStatus(StatusState::Busy, text);
}

void CoreAnalyzePage::onFinished()
{
    auto *thread = qobject_cast<AnalyzeThread *>(sender());
    if (!thread || thread != m_thread)
        return;
    const AnalysisReport report = thread->report();
    m_thread = nullptr;
    thread->deleteLater();
    m_running = false;
    m_lock.release();
    showReport(report);
    updateUi();
}

void CoreAnalyzePage::showReport(const AnalysisReport &report)
{
    m_error->setText(report.errorInfo.isEmpty() ? QStringLiteral("无") : report.errorInfo);
    m_where->setText(report.whereInfo.isEmpty() ? QStringLiteral("无") : report.whereInfo);
    m_cause->setText(report.cause.isEmpty() ? QStringLiteral("无") : report.cause);
    m_suggestion->setText(report.suggestionText.isEmpty() ? QStringLiteral("无") : report.suggestionText);
    if (!report.rendered.isEmpty())
        m_report->setPlainText(report.rendered);

    const QString dir = dataDirectory();
    if (!dir.isEmpty() && !report.rendered.isEmpty()) {
        QString ignored;
        QFile out(QDir(dir).filePath(QStringLiteral("last-report.txt")));
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            out.write(report.rendered.toUtf8());
        else
            ignored = out.errorString();
        Q_UNUSED(ignored);
    }

    if (report.cancelled) {
        m_progress->setText(QStringLiteral("已取消"));
        m_services.showStatus(StatusState::Info, QStringLiteral("已取消分析"));
        m_services.logWarning(QStringLiteral("分析已取消"));
        return;
    }
    if (report.ok) {
        m_progress->setText(QStringLiteral("分析完成"));
        const QString brief = report.signalName.isEmpty()
            ? QStringLiteral("分析完成")
            : QStringLiteral("分析完成：%1").arg(report.signalName);
        m_services.showStatus(StatusState::Success, brief);
        m_services.logSuccess(brief);
        return;
    }
    m_progress->setText(QStringLiteral("分析结束"));
    m_services.showStatus(StatusState::Error, QStringLiteral("分析失败"));
    m_services.logError(report.errorInfo);
}

void CoreAnalyzePage::updateUi()
{
    const auto buttons = findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button == m_cancel)
            button->setEnabled(m_running);
        else if (button == m_export)
            button->setEnabled(!m_running && !m_report->toPlainText().isEmpty());
        else
            button->setEnabled(!m_running);
    }
    m_coreEdit->setEnabled(!m_running);
    m_exeEdit->setEnabled(!m_running);
    m_symbolEdit->setEnabled(!m_running);
    if (m_running) {
        m_bar->setRange(0, 0);
    } else {
        m_bar->setRange(0, 1);
        m_bar->setValue(m_progress->text() == QStringLiteral("分析完成") ? 1 : 0);
    }
}

ToolInfo CoreAnalyzeTool::info() const
{
    ToolInfo info;
    info.id = QStringLiteral("core-analyze");
    info.title = QStringLiteral("文件分析");
    info.summary = QStringLiteral("查看 Linux core 文件为何停止，并写出能看懂的说明");
    info.iconKey = QStringLiteral("status");
    info.order = 40;
    return info;
}

bool CoreAnalyzeTool::initialize(const ToolContext &context, QString *error)
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

QVector<ToolPage> CoreAnalyzeTool::createPages()
{
    auto *page = new CoreAnalyzePage(m_context.services, m_context.dialogParent);
    m_page = page;
    QVector<ToolPage> pages;
    pages.append({PageInfo{QStringLiteral("analyze"), QStringLiteral("分析"),
                           QStringLiteral("查看 core 文件并导出说明"), QStringLiteral("status")},
                  page});
    return pages;
}

void CoreAnalyzeTool::shutdown()
{
    if (m_page)
        m_page->abort();
}

} // namespace tw
