#pragma once

#include "shell/i_tool.h"

#include <QPointer>
#include <QWidget>

#include <memory>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableView;
class QThread;

namespace tw {

class FilterUniqueFileModel;
class FilterUniqueProblemModel;
class FilterUniqueProxy;
struct FilterDeleteResult;
struct FilterJob;
struct FilterScanResult;

// 筛选去重的页面。长任务在后台线程跑，期间持有导航锁；删除必须先预览再二次确认。
class FilterUniquePage : public QWidget
{
    Q_OBJECT

public:
    FilterUniquePage(const ToolServices &services, const QString &dataDir, QWidget *dialogParent,
                     QWidget *parent = nullptr);
    ~FilterUniquePage() override;

    void abort();
    bool isBusy() const { return m_busy; }
    void setStepDelayMs(int milliseconds) { m_stepDelayMs = qMax(0, milliseconds); }
    bool importKeywordsFromFile(const QString &path, QString *error);

private slots:
    void setBusyDetail(const QString &text);

private:
    enum class PreviewStage { Hidden, Review, Confirm };

    void buildUi();
    void loadSession();
    void saveSession() const;
    void refreshKeywordCount();
    void startScan();
    void cancelTask();
    void browseDirectory();
    void importFromDialog();
    void onDirectoryEdited(const QString &text);
    void updateVisibleCount();
    void recount();
    void updateUi();
    void beginPreview(bool matched);
    void continuePreview();
    void backToReview();
    void cancelPreview();
    void confirmDelete();
    void startJob(const std::shared_ptr<FilterJob> &job, const QString &lockReason,
                  const QString &statusText);
    void onThreadFinished();
    void applyScan(const FilterScanResult &result);
    void applyDelete(const FilterDeleteResult &result);
    void clearResults();
    void setPreviewStage(PreviewStage stage);
    QStringList pathsFor(bool matched) const;

    ToolServices m_services;
    QString m_dataDir;
    QString m_scannedRoot;
    QPointer<QWidget> m_dialogParent;
    NavigationLock m_lock;
    std::shared_ptr<FilterJob> m_job;
    QThread *m_thread = nullptr;
    int m_stepDelayMs = 0;
    bool m_busy = false;
    bool m_aborting = false;
    bool m_loading = false;
    PreviewStage m_previewStage = PreviewStage::Hidden;
    bool m_previewMatched = true;
    QStringList m_pendingPaths;

    int m_matched = 0;
    int m_unmatched = 0;
    int m_total = 0;
    int m_problemCount = 0;

    QPlainTextEdit *m_keywords = nullptr;
    QPushButton *m_import = nullptr;
    QLabel *m_keywordCount = nullptr;
    QLineEdit *m_directory = nullptr;
    QPushButton *m_browse = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_cancel = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_detail = nullptr;
    QLabel *m_rule = nullptr;
    QLabel *m_matchedLabel = nullptr;
    QLabel *m_unmatchedLabel = nullptr;
    QLabel *m_totalLabel = nullptr;
    QLabel *m_problemLabel = nullptr;
    QLineEdit *m_search = nullptr;
    QComboBox *m_mode = nullptr;
    QLabel *m_visibleCount = nullptr;
    QTableView *m_results = nullptr;
    QTableView *m_problems = nullptr;
    FilterUniqueFileModel *m_fileModel = nullptr;
    FilterUniqueProxy *m_proxy = nullptr;
    FilterUniqueProblemModel *m_problemModel = nullptr;
    QPushButton *m_deleteMatched = nullptr;
    QPushButton *m_deleteUnmatched = nullptr;
    QFrame *m_preview = nullptr;
    QLabel *m_previewTitle = nullptr;
    QLabel *m_previewHint = nullptr;
    QListWidget *m_previewList = nullptr;
    QPushButton *m_previewCancel = nullptr;
    QPushButton *m_previewContinue = nullptr;
    QPushButton *m_previewBack = nullptr;
    QPushButton *m_confirmDelete = nullptr;
};

class FilterUniqueTool : public ITool
{
public:
    ToolInfo info() const override;
    bool initialize(const ToolContext &context, QString *error) override;
    QVector<ToolPage> createPages() override;
    void shutdown() override;

    FilterUniquePage *page() const { return m_page; }

private:
    ToolContext m_context;
    QString m_dataDir;
    QPointer<FilterUniquePage> m_page;
};

} // namespace tw
