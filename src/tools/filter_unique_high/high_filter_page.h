#pragma once

#include "core/platform_services.h"
#include "shell/i_tool.h"

#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTableView;
class QThread;

namespace tw {

class ElidedLabel;
class HitModel;
class HitProxy;
class IssueModel;
struct Job;
struct RemovalBatch;
struct TreeWalk;

// 筛选去重 high 的工作页。扫描和删除在后台线程进行，期间持有导航锁。
// 删除必须先看完整列表，再做第二次确认；这两步之前不会改目录。
class HighFilterPage : public QWidget
{
    Q_OBJECT

public:
    HighFilterPage(const ToolServices &services, const QString &dataDir, QWidget *dialogParent,
                   QWidget *parent = nullptr);
    ~HighFilterPage() override;

    void abort();
    bool isBusy() const { return m_busy; }
    void setPaceMs(int milliseconds) { m_paceMs = milliseconds < 0 ? 0 : milliseconds; }
    bool loadTermsFromFile(const QString &path, QString *error);

private:
    enum class Stage { Hidden, Review, Confirm };

    void buildUi();
    void loadState();
    void saveState() const;
    void refreshTermCount();
    void startScan();
    void cancelTask();
    void browseDirectory();
    void importFromDialog();
    void onTermsEdited();
    void onDirectoryEdited();
    void onSearchChanged(const QString &text);
    void onModeChanged(int index);
    void recount();
    void updateVisible();
    void updateControls();
    void beginPreview(bool matched);
    void continuePreview();
    void backToReview();
    void cancelPreview();
    void confirmDelete();
    void startJob(const std::shared_ptr<Job> &job, const QString &lockReason,
                  const QString &statusText);
    void onThreadFinished();
    void applyWalk(const TreeWalk &result);
    void applyRemoval(const RemovalBatch &result);
    void clearResults(const QString &detail);
    void setStage(Stage stage);
    void showPace(int files, int issues);
    void showRemovalPace(int done, int total);

    ToolServices m_services;
    QString m_dataDir;
    QString m_scannedRoot;
    QPointer<QWidget> m_dialogParent;
    NavigationLock m_lock;
    std::shared_ptr<Job> m_job;
    QThread *m_thread = nullptr;
    int m_paceMs = 0;
    bool m_busy = false;
    bool m_aborting = false;
    bool m_loading = false;
    Stage m_stage = Stage::Hidden;
    bool m_previewMatched = true;
    QStringList m_pending;
    int m_matched = 0;
    int m_unmatched = 0;
    int m_issueCount = 0;

    QPlainTextEdit *m_terms = nullptr;
    QLabel *m_termCount = nullptr;
    QPushButton *m_import = nullptr;
    QLineEdit *m_directory = nullptr;
    QPushButton *m_browse = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_cancel = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_detail = nullptr;
    ElidedLabel *m_rule = nullptr;
    QLabel *m_matchedCount = nullptr;
    QLabel *m_unmatchedCount = nullptr;
    QLabel *m_totalCount = nullptr;
    QLabel *m_issueLabel = nullptr;
    QLineEdit *m_search = nullptr;
    QComboBox *m_viewMode = nullptr;
    QLabel *m_visibleCount = nullptr;
    QTableView *m_results = nullptr;
    QTableView *m_issues = nullptr;
    HitModel *m_files = nullptr;
    HitProxy *m_proxy = nullptr;
    IssueModel *m_issueModel = nullptr;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_preview = nullptr;
    QLabel *m_previewTitle = nullptr;
    QLabel *m_previewHint = nullptr;
    QListWidget *m_previewList = nullptr;
    QPushButton *m_deleteMatched = nullptr;
    QPushButton *m_deleteUnmatched = nullptr;
    QPushButton *m_previewCancel = nullptr;
    QPushButton *m_previewBack = nullptr;
    QPushButton *m_previewContinue = nullptr;
    QPushButton *m_confirmDelete = nullptr;
};

} // namespace tw
