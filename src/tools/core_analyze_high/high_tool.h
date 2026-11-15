#pragma once

#include "shell/i_tool.h"
#include "tools/core_analyze_high/triage.h"

#include <QPointer>
#include <QThread>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>

class QDialog;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSplitter;
class QTableWidget;

namespace tw {

using ArgvHostFactory = std::function<std::unique_ptr<ArgvHost>(std::atomic<bool> *)>;

// 后台线程只跑分析。QThread 对象本身留在界面线程，结束后再读结果。
class TriageThread : public QThread
{
    Q_OBJECT

public:
    TriageThread(TriageInput input, ToolLocator locator, ArgvHostFactory factory,
                 QObject *parent = nullptr);

    void askStop() { m_stop.store(true); }
    TriageOutcome outcome() const { return m_outcome; }

signals:
    void note(const QString &text);

protected:
    void run() override;

private:
    std::atomic<bool> m_stop{false};
    TriageInput m_input;
    ToolLocator m_locator;
    ArgvHostFactory m_factory;
    TriageOutcome m_outcome;
};

class HighAnalyzePage : public QWidget
{
    Q_OBJECT

public:
    HighAnalyzePage(const ToolServices &services, QWidget *dialogParent, QWidget *parent = nullptr);
    ~HighAnalyzePage() override;

    void abort();
    void setHostFactoryForTest(const ArgvHostFactory &factory) { m_factory = factory; }
    void setLocatorForTest(const ToolLocator &locator) { m_locator = locator; }

    // 把当前说明写成 UTF-8 文本（不带 BOM）。
    bool saveReport(const QString &path, QString *error = nullptr) const;

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void browseFile(QLineEdit *edit, const QString &title);
    void browseDebugDirectory();
    void loadPaths();
    void storePaths() const;
    QString dataDirectory() const;
    void start();
    void cancel();
    void exportReport();
    void onNote(const QString &text);
    void onFinished();
    void showOutcome(const TriageOutcome &outcome);
    void refreshControls();
    void openConditions();
    void showSteps(const QVector<TriageStep> &steps, const QStringList &markers);
    void fillPreviewSteps();
    void focusStep(int index);
    void openStepDialog(int index);
    void openValueDialog(QLabel *label);
    void jumpFromLabel(QLabel *label);
    void revealInReport(const QString &marker);
    void findInReport();
    void copyCurrentCommand();
    void showTextDialog(const QString &title, const QString &body);
    void setProgressText(const QString &text);
    void ensurePanes();
    void onStepMenu(const QPoint &pos);

    ToolServices m_services;
    QPointer<QWidget> m_dialogParent;
    ArgvHostFactory m_factory;
    ToolLocator m_locator;
    NavigationLock m_lock;
    QPointer<TriageThread> m_thread;
    bool m_running = false;
    bool m_closing = false;
    bool m_adjusting = false;
    bool m_userSized = false;

    QLineEdit *m_coreEdit = nullptr;
    QLineEdit *m_programEdit = nullptr;
    QLineEdit *m_debugEdit = nullptr;
    QLineEdit *m_findEdit = nullptr;
    QPushButton *m_run = nullptr;
    QPushButton *m_stop = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_copy = nullptr;
    QProgressBar *m_bar = nullptr;
    QLabel *m_progress = nullptr;
    QLabel *m_fault = nullptr;
    QLabel *m_place = nullptr;
    QLabel *m_likely = nullptr;
    QLabel *m_advice = nullptr;
    QPlainTextEdit *m_command = nullptr;
    QPlainTextEdit *m_detail = nullptr;
    QTableWidget *m_steps = nullptr;
    QSplitter *m_vertical = nullptr;
    QSplitter *m_columns = nullptr;
    QSplitter *m_detailSplit = nullptr;
    QDialog *m_conditions = nullptr;
    QVector<TriageStep> m_shown;
    QStringList m_markers;
    QString m_progressFull;
};

class HighAnalyzeTool : public ITool
{
public:
    ToolInfo info() const override;
    bool initialize(const ToolContext &context, QString *error) override;
    QVector<ToolPage> createPages() override;
    void shutdown() override;

    HighAnalyzePage *page() const { return m_page; }

private:
    ToolContext m_context;
    QPointer<HighAnalyzePage> m_page;
};

} // namespace tw
