#pragma once

#include "core/platform_services.h"
#include "shell/i_tool.h"
#include "tools/md5_compare_high/high_digest.h"

#include <QPointer>
#include <QWidget>

#include <atomic>
#include <thread>

class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QSortFilterProxyModel;
class QSplitter;
class QTableView;
class QTreeView;

namespace tw {

class ElidedLabel;
class HighResultModel;
class HighTreeModel;

// 目录比较页。扫描和哈希在后台线程进行，界面线程只接收进度和最终结果。
// 比较期间持有导航锁；成功、失败、取消和关闭都会释放。
class Md5CompareHighPage : public QWidget
{
    Q_OBJECT

public:
    explicit Md5CompareHighPage(const ToolServices &services, QWidget *dialogParent = nullptr,
                                QWidget *parent = nullptr);
    ~Md5CompareHighPage() override;

    bool isBusy() const { return m_running; }
    void stopWork();

    // 把最近一次成功的比较写成 CSV。路径落在被比较目录内时拒绝。
    bool exportReportTo(const QString &path, QString *error = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void browse(QLineEdit *edit, const QString &title);
    void startCompare();
    void cancelCompare();
    void chooseExport();
    void finishCompare(quint64 generation, bool ok, const HighReport &report, const QString &error);
    void applyProgress(const HighProgress &progress);
    void showReport(const HighReport &report);
    void clearPresentation();
    void refilter();
    void updateActions();
    void loadMemory();
    void saveMemory();
    QString memoryFile() const;
    void syncToPath(const QString &relativePath);
    void openDetail(const QString &relativePath);
    void revealPath(QTreeView *tree, const QString &relativePath);
    void revealInTable(const QString &relativePath);
    void fillDetail(const QString &relativePath);
    void applySplitterSizes();

    ToolServices m_services;
    QWidget *m_dialogParent = nullptr;
    NavigationLock m_lock;
    HighReport m_report;
    HighResultModel *m_model = nullptr;
    HighTreeModel *m_treeModel = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    std::thread m_job;
    std::atomic<bool> m_stopRequested{false};
    quint64 m_generation = 0;
    bool m_running = false;
    bool m_hasReport = false;
    bool m_discardOutcome = false;
    bool m_closing = false;
    bool m_syncing = false;
    bool m_splitSized = false;
    QString m_rootA;
    QString m_rootB;

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
    QComboBox *m_filter = nullptr;
    QLineEdit *m_search = nullptr;
    QSplitter *m_splitV = nullptr;
    QSplitter *m_splitH = nullptr;
    QTreeView *m_treeA = nullptr;
    QTreeView *m_treeB = nullptr;
    QTableView *m_table = nullptr;
    QPlainTextEdit *m_problems = nullptr;
    ElidedLabel *m_preview = nullptr;
    QDialog *m_detail = nullptr;
    QLabel *m_detailPath = nullptr;
    QLabel *m_detailKind = nullptr;
    QLabel *m_detailStatus = nullptr;
    QLabel *m_detailMd5A = nullptr;
    QLabel *m_detailMd5B = nullptr;
    QLabel *m_detailNote = nullptr;
    QLabel *m_detailAbsA = nullptr;
    QLabel *m_detailAbsB = nullptr;
};

class Md5CompareHighTool : public ITool
{
public:
    ToolInfo info() const override;
    bool initialize(const ToolContext &context, QString *error) override;
    QVector<ToolPage> createPages() override;
    void shutdown() override;

    Md5CompareHighPage *page() const { return m_page; }

private:
    ToolContext m_context;
    QPointer<Md5CompareHighPage> m_page;
};

} // namespace tw
