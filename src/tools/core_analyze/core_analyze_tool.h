#pragma once

#include "tools/core_analyze/core_analyzer.h"
#include "shell/i_tool.h"

#include <QPointer>
#include <QThread>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QWidget;

namespace tw {

using ExecutorFactory = std::function<std::unique_ptr<CommandExecutor>(std::atomic<bool> *)>;

class AnalyzeThread : public QThread
{
    Q_OBJECT

public:
    void setRequest(const AnalysisRequest &request) { m_request = request; }
    void setFinder(const ToolFinder &finder) { m_finder = finder; }
    void setFactory(const ExecutorFactory &factory) { m_factory = factory; }
    void requestCancel() { m_cancel.store(true); }
    AnalysisReport report() const { return m_report; }

signals:
    void progressText(const QString &text);

protected:
    void run() override;

private:
    std::atomic<bool> m_cancel{false};
    AnalysisRequest m_request;
    ToolFinder m_finder;
    ExecutorFactory m_factory;
    AnalysisReport m_report;
};

class CoreAnalyzePage : public QWidget
{
    Q_OBJECT

public:
    CoreAnalyzePage(const ToolServices &services, QWidget *dialogParent, QWidget *parent = nullptr);
    ~CoreAnalyzePage() override;

    void abort();
    void setExecutorFactoryForTest(const ExecutorFactory &factory) { m_factory = factory; }
    void setFinderForTest(const ToolFinder &finder) { m_finder = finder; }

    // 把当前说明写成 UTF-8 文本。供导出按钮和测试使用。
    bool saveReport(const QString &path, QString *error = nullptr) const;

private:
    void browseFile(QLineEdit *edit, const QString &title);
    void browseSymbolDir();
    void loadInputs();
    void saveInputs() const;
    QString dataDirectory() const;
    void start();
    void cancel();
    void exportReport();
    void onProgress(const QString &text);
    void onFinished();
    void showReport(const AnalysisReport &report);
    void updateUi();

    ToolServices m_services;
    QPointer<QWidget> m_dialogParent;
    ExecutorFactory m_factory;
    ToolFinder m_finder;
    NavigationLock m_lock;
    AnalyzeThread *m_thread = nullptr;
    bool m_running = false;

    QLineEdit *m_coreEdit = nullptr;
    QLineEdit *m_exeEdit = nullptr;
    QLineEdit *m_symbolEdit = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_export = nullptr;
    QProgressBar *m_bar = nullptr;
    QLabel *m_progress = nullptr;
    QLabel *m_error = nullptr;
    QLabel *m_where = nullptr;
    QLabel *m_cause = nullptr;
    QLabel *m_suggestion = nullptr;
    QPlainTextEdit *m_report = nullptr;
};

class CoreAnalyzeTool : public ITool
{
public:
    ToolInfo info() const override;
    bool initialize(const ToolContext &context, QString *error) override;
    QVector<ToolPage> createPages() override;
    void shutdown() override;

    CoreAnalyzePage *page() const { return m_page; }

private:
    ToolContext m_context;
    QPointer<CoreAnalyzePage> m_page;
};

} // namespace tw
