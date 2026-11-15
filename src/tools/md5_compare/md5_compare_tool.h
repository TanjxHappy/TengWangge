#pragma once

#include "core/platform_services.h"
#include "shell/i_tool.h"
#include "tools/md5_compare/compare_engine.h"

#include <QPointer>
#include <QWidget>

#include <atomic>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableView;

namespace tw {

class CompareResultModel;
class CompareThread;
class ElidedLabel;
class ExportThread;

// 目录 MD5 比较页。比较在后台线程进行，期间持有导航锁。
class Md5ComparePage : public QWidget
{
    Q_OBJECT

public:
    explicit Md5ComparePage(const ToolServices &services, QWidget *parent = nullptr);
    ~Md5ComparePage() override;

    bool isBusy() const { return m_running; }
    void abort();
    // 把当前比较结果写到 path。对话框由导出按钮负责；测试可以直接调用。
    void exportTo(const QString &path);

private:
    void browse(QLineEdit *edit, const QString &title);
    void startCompare();
    void cancelCompare();
    void exportReport();
    void onProgress(bool enumerating, int completed, int total, const QString &path);
    void onFinished();
    void onExportFinished();
    void applyFilter();
    void updateActions();
    void loadState();
    void saveState();
    QString statePath() const;
    void showFailure(const QString &message);

    ToolServices m_services;
    NavigationLock m_lock;
    CompareResult m_result;
    CompareResultModel *m_model = nullptr;
    CompareThread *m_thread = nullptr;
    ExportThread *m_exportThread = nullptr;
    std::atomic<bool> m_cancelRequested{false};
    bool m_running = false;
    bool m_hasResult = false;
    QString m_activeA;
    QString m_activeB;
    QString m_comparedA;
    QString m_comparedB;

    QLineEdit *m_dirA = nullptr;
    QLineEdit *m_dirB = nullptr;
    QPushButton *m_browseA = nullptr;
    QPushButton *m_browseB = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_export = nullptr;
    QProgressBar *m_bar = nullptr;
    ElidedLabel *m_state = nullptr;
    ElidedLabel *m_counts = nullptr;
    QLabel *m_visible = nullptr;
    QComboBox *m_filter = nullptr;
    QLineEdit *m_search = nullptr;
    QTableView *m_table = nullptr;
    QPlainTextEdit *m_problems = nullptr;
};

class Md5CompareTool : public ITool
{
public:
    ToolInfo info() const override;
    bool initialize(const ToolContext &context, QString *error) override;
    QVector<ToolPage> createPages() override;
    void shutdown() override;

    Md5ComparePage *page() const { return m_page; }

private:
    ToolContext m_context;
    QPointer<Md5ComparePage> m_page;
};

} // namespace tw
