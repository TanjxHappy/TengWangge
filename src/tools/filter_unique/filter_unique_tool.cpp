#include "tools/filter_unique/filter_unique_tool.h"

#include "tools/filter_unique/filter_unique_scan.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTableView>
#include <QThread>
#include <QVBoxLayout>

#include <chrono>
#include <thread>
#include <utility>

namespace tw {
namespace {

QString matchRuleText()
{
    return QStringLiteral(
        "匹配规则：每行一个名称或关键词，忽略空行，重复的关键词只保留第一次出现的写法。"
        "递归扫描所选目录中的文件，把相对路径和文件名与关键词做子串比较，不区分大小写；"
        "命中任意一个关键词即算匹配。不比较所选目录本身的绝对路径，也不读取文件内容。"
        "符号链接不会跟到起始目录之外，循环链接会跳过；目录符号链接不展开，避免把同一批文件算两遍。"
        "无法读取的项只记入问题列表，不会被删除。"
        "只有完成预览和第二次确认后才会删除文件；未确认不会改动该目录，也不会删除目录本身。");
}

QLabel *makeLabel(const QString &text, const char *role)
{
    auto *label = new QLabel(text);
    label->setProperty("role", QString::fromLatin1(role));
    label->setWordWrap(true);
    return label;
}

QLabel *makeCount(const QString &objectName, const QString &accessibleName)
{
    auto *label = makeLabel(QStringLiteral("0"), "value");
    label->setObjectName(objectName);
    label->setAccessibleName(accessibleName);
    return label;
}

} // namespace

class FilterUniqueFileModel : public QAbstractTableModel
{
public:
    explicit FilterUniqueFileModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : m_rows.size();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : 3;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        switch (section) {
        case 0:
            return QStringLiteral("相对路径");
        case 1:
            return QStringLiteral("结果");
        case 2:
            return QStringLiteral("匹配到的名单项");
        default:
            return {};
        }
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
            return {};
        const FilterFileRow &row = m_rows.at(index.row());
        if (role == Qt::ToolTipRole && index.column() == 0)
            return row.absolutePath;
        if (role == Qt::UserRole && index.column() == 0)
            return row.relativePath + QLatin1Char('\n') + row.matchedTerms.join(QLatin1Char('\n'));
        if (role == Qt::UserRole + 1 && index.column() == 0)
            return row.matched;
        if (role != Qt::DisplayRole)
            return {};
        switch (index.column()) {
        case 0:
            return row.relativePath;
        case 1:
            return row.matched ? QStringLiteral("匹配") : QStringLiteral("不匹配");
        case 2:
            return row.matchedTerms.isEmpty() ? QStringLiteral("无")
                                              : row.matchedTerms.join(QStringLiteral("、"));
        default:
            return {};
        }
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!index.isValid())
            return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

    void setRows(const QVector<FilterFileRow> &rows)
    {
        beginResetModel();
        m_rows = rows;
        endResetModel();
    }

    const QVector<FilterFileRow> &rows() const { return m_rows; }

private:
    QVector<FilterFileRow> m_rows;
};

class FilterUniqueProblemModel : public QAbstractTableModel
{
public:
    explicit FilterUniqueProblemModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : m_rows.size();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : 2;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        switch (section) {
        case 0:
            return QStringLiteral("路径");
        case 1:
            return QStringLiteral("原因");
        default:
            return {};
        }
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
            return {};
        const FilterProblem &row = m_rows.at(index.row());
        if (role == Qt::ToolTipRole)
            return index.column() == 0 ? QVariant(row.path) : QVariant(row.reason);
        if (role != Qt::DisplayRole)
            return {};
        return index.column() == 0 ? QVariant(row.path) : QVariant(row.reason);
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!index.isValid())
            return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

    void setProblems(const QVector<FilterProblem> &rows)
    {
        beginResetModel();
        m_rows = rows;
        endResetModel();
    }

    void addProblems(const QVector<FilterProblem> &extra)
    {
        if (extra.isEmpty())
            return;
        beginResetModel();
        m_rows += extra;
        endResetModel();
    }

    const QVector<FilterProblem> &problems() const { return m_rows; }

private:
    QVector<FilterProblem> m_rows;
};

class FilterUniqueProxy : public QSortFilterProxyModel
{
public:
    explicit FilterUniqueProxy(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
        setFilterCaseSensitivity(Qt::CaseInsensitive);
        setFilterRole(Qt::UserRole);
        setFilterKeyColumn(0);
        setDynamicSortFilter(true);
    }

    void setMode(int mode)
    {
        m_mode = mode;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        if (!QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent))
            return false;
        if (m_mode == 0)
            return true;
        const bool matched = sourceModel()->index(sourceRow, 0, sourceParent).data(Qt::UserRole + 1).toBool();
        return m_mode == 1 ? matched : !matched;
    }

private:
    int m_mode = 0;
};

struct FilterJob
{
    enum class Kind { Scan, Delete };

    Kind kind = Kind::Scan;
    std::atomic<bool> cancel{false};
    int stepDelayMs = 0;
    QString root;
    QStringList keywords;
    QStringList paths;
    FilterScanResult scanResult;
    FilterDeleteResult deleteResult;
    std::function<void(int, int)> progress;
};

class FilterUniqueThread : public QThread
{
public:
    explicit FilterUniqueThread(std::shared_ptr<FilterJob> job)
        : m_job(std::move(job))
    {
        setObjectName(QStringLiteral("filter-unique-job"));
    }

protected:
    void run() override
    {
        if (!m_job)
            return;
        if (m_job->stepDelayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(m_job->stepDelayMs));
        if (m_job->cancel.load()) {
            if (m_job->kind == FilterJob::Kind::Scan)
                m_job->scanResult.cancelled = true;
            else
                m_job->deleteResult.cancelled = true;
            return;
        }
        if (m_job->kind == FilterJob::Kind::Scan) {
            m_job->scanResult = scanDirectory(m_job->root, m_job->keywords, m_job->cancel, m_job->progress);
        } else {
            m_job->deleteResult = deleteListedFiles(m_job->root, m_job->paths, m_job->cancel, m_job->progress);
        }
    }

private:
    std::shared_ptr<FilterJob> m_job;
};

FilterUniquePage::FilterUniquePage(const ToolServices &services, const QString &dataDir,
                                   QWidget *dialogParent, QWidget *parent)
    : QWidget(parent)
    , m_services(services)
    , m_dataDir(dataDir)
    , m_dialogParent(dialogParent)
{
    buildUi();
    loadSession();
    refreshKeywordCount();
    updateUi();
}

FilterUniquePage::~FilterUniquePage()
{
    abort();
}

void FilterUniquePage::abort()
{
    saveSession();
    m_aborting = true;
    if (m_job)
        m_job->cancel.store(true);
    if (m_thread && m_thread->isRunning())
        m_thread->wait(30000);
    if (m_thread) {
        QThread *thread = m_thread;
        m_thread = nullptr;
        if (thread->isRunning())
            thread->wait(5000);
        if (!thread->isRunning())
            thread->deleteLater();
        else
            m_services.logError(QStringLiteral("筛选任务未能及时结束"));
    }
    m_job.reset();
    m_busy = false;
    m_lock.release();
}

bool FilterUniquePage::importKeywordsFromFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        const QString message = QStringLiteral("无法读取名单文件：%1").arg(file.errorString());
        if (error)
            *error = message;
        return false;
    }
    constexpr qint64 kMaxBytes = 5 * 1024 * 1024;
    if (file.size() > kMaxBytes) {
        const QString message = QStringLiteral("名单文件过大（超过 5 MB）");
        if (error)
            *error = message;
        return false;
    }
    QByteArray data = file.readAll();
    if (data.startsWith("\xEF\xBB\xBF"))
        data.remove(0, 3);
    m_keywords->setPlainText(QString::fromUtf8(data));
    refreshKeywordCount();
    saveSession();
    m_services.logInfo(QStringLiteral("已导入筛选名单"));
    m_services.showStatus(StatusState::Info, QStringLiteral("已导入筛选名单"));
    return true;
}

void FilterUniquePage::buildUi()
{
    setAccessibleName(QStringLiteral("筛选去重"));
    setObjectName(QStringLiteral("filterUnique.page"));

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("filterUnique.scroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setAccessibleName(QStringLiteral("筛选去重内容"));
    scroll->viewport()->setAutoFillBackground(false);

    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(12);
    scroll->setWidget(content);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    layout->addWidget(makeLabel(QStringLiteral("筛选去重"), "pageTitle"));
    layout->addWidget(makeLabel(
        QStringLiteral("按名单模糊筛选目录中的文件。删除前会预览列表并二次确认。"), "pageSubtitle"));
    m_rule = makeLabel(matchRuleText(), "hint");
    m_rule->setObjectName(QStringLiteral("filterUnique.rule"));
    m_rule->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *form = new QFrame;
    form->setProperty("role", QStringLiteral("card"));
    auto *grid = new QGridLayout(form);
    grid->setContentsMargins(16, 14, 16, 14);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(0, 1);

    grid->addWidget(makeLabel(QStringLiteral("筛选名单"), "fieldLabel"), 0, 0);

    m_keywords = new QPlainTextEdit;
    m_keywords->setObjectName(QStringLiteral("filterUnique.keywords"));
    m_keywords->setPlaceholderText(QStringLiteral("每行一个名称或关键词，可直接粘贴"));
    m_keywords->setMinimumHeight(64);
    m_keywords->setMaximumHeight(96);
    m_keywords->setAccessibleName(QStringLiteral("筛选名单，每行一个名称或关键词"));
    m_keywords->setTabChangesFocus(true);
    m_keywords->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_import = new QPushButton(QStringLiteral("导入文本…"));
    m_import->setObjectName(QStringLiteral("filterUnique.import"));
    m_import->setAccessibleName(QStringLiteral("导入筛选名单"));
    m_import->setToolTip(QStringLiteral("用 UTF-8 文本替换当前名单，每行一个关键词"));

    grid->addWidget(m_keywords, 1, 0);
    grid->addWidget(m_import, 1, 1, Qt::AlignTop);

    m_keywordCount = makeLabel(QStringLiteral("已识别 0 个关键词"), "hint");
    m_keywordCount->setObjectName(QStringLiteral("filterUnique.keywordCount"));
    grid->addWidget(m_keywordCount, 2, 0, 1, 2);

    grid->addWidget(makeLabel(QStringLiteral("目录"), "fieldLabel"), 3, 0);

    m_directory = new QLineEdit;
    m_directory->setObjectName(QStringLiteral("filterUnique.directory"));
    m_directory->setPlaceholderText(QStringLiteral("/data/project"));
    m_directory->setAccessibleName(QStringLiteral("要筛选的目录，必须是已存在的绝对路径"));
    m_directory->setClearButtonEnabled(true);

    m_browse = new QPushButton(QStringLiteral("浏览…"));
    m_browse->setObjectName(QStringLiteral("filterUnique.browse"));
    m_browse->setAccessibleName(QStringLiteral("浏览目录"));

    auto *dirRow = new QWidget;
    auto *dirLayout = new QHBoxLayout(dirRow);
    dirLayout->setContentsMargins(0, 0, 0, 0);
    dirLayout->setSpacing(8);
    dirLayout->addWidget(m_directory, 1);
    dirLayout->addWidget(m_browse);
    grid->addWidget(dirRow, 4, 0, 1, 2);

    grid->addWidget(makeLabel(
                         QStringLiteral("必须是已存在的绝对路径。扫描本身不会修改该目录。"), "hint"),
                     5, 0, 1, 2);

    m_start = new QPushButton(QStringLiteral("开始筛选"));
    m_start->setObjectName(QStringLiteral("filterUnique.start"));
    m_start->setProperty("role", QStringLiteral("primary"));
    m_start->setAccessibleName(QStringLiteral("开始筛选"));
    m_start->setDefault(true);

    m_cancel = new QPushButton(QStringLiteral("取消"));
    m_cancel->setObjectName(QStringLiteral("filterUnique.cancel"));
    m_cancel->setAccessibleName(QStringLiteral("取消当前筛选或删除"));
    m_cancel->setAutoDefault(false);

    m_progress = new QProgressBar;
    m_progress->setObjectName(QStringLiteral("filterUnique.progress"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setAccessibleName(QStringLiteral("筛选进度"));
    m_progress->setFocusPolicy(Qt::NoFocus);
    m_progress->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_detail = makeLabel(QStringLiteral("尚未筛选"), "hint");
    m_detail->setObjectName(QStringLiteral("filterUnique.detail"));

    auto *actions = new QWidget;
    auto *actionLayout = new QHBoxLayout(actions);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(8);
    actionLayout->addWidget(m_start);
    actionLayout->addWidget(m_cancel);
    actionLayout->addWidget(m_progress, 1);
    actionLayout->addWidget(m_detail, 1);
    grid->addWidget(actions, 6, 0, 1, 2);
    layout->addWidget(form);
    layout->addWidget(m_rule);

    auto *summary = new QHBoxLayout;
    summary->setSpacing(16);
    auto addCount = [&](QLabel **value, const QString &objectName, const QString &caption,
                        const QString &accessibleName) {
        auto *pair = new QHBoxLayout;
        pair->setSpacing(6);
        pair->addWidget(makeLabel(caption, "fieldLabel"));
        *value = makeCount(objectName, accessibleName);
        pair->addWidget(*value);
        summary->addLayout(pair);
    };
    addCount(&m_matchedLabel, QStringLiteral("filterUnique.matchedCount"), QStringLiteral("匹配"),
             QStringLiteral("匹配数"));
    addCount(&m_unmatchedLabel, QStringLiteral("filterUnique.unmatchedCount"), QStringLiteral("不匹配"),
             QStringLiteral("不匹配数"));
    addCount(&m_totalLabel, QStringLiteral("filterUnique.totalCount"), QStringLiteral("总计"),
             QStringLiteral("文件总数"));
    addCount(&m_problemLabel, QStringLiteral("filterUnique.problemCount"), QStringLiteral("问题"),
             QStringLiteral("问题数"));
    summary->addStretch(1);
    layout->addLayout(summary);

    m_deleteMatched = new QPushButton(QStringLiteral("删除匹配的文件（0）"));
    m_deleteMatched->setObjectName(QStringLiteral("filterUnique.deleteMatched"));
    m_deleteMatched->setProperty("role", QStringLiteral("danger"));
    m_deleteMatched->setAccessibleName(QStringLiteral("删除匹配的文件"));
    m_deleteMatched->setAutoDefault(false);

    m_deleteUnmatched = new QPushButton(QStringLiteral("删除不匹配的文件（0）"));
    m_deleteUnmatched->setObjectName(QStringLiteral("filterUnique.deleteUnmatched"));
    m_deleteUnmatched->setProperty("role", QStringLiteral("danger"));
    m_deleteUnmatched->setAccessibleName(QStringLiteral("删除不匹配的文件"));
    m_deleteUnmatched->setAutoDefault(false);

    auto *deleteRow = new QHBoxLayout;
    deleteRow->setSpacing(8);
    deleteRow->addWidget(m_deleteMatched);
    deleteRow->addWidget(m_deleteUnmatched);
    deleteRow->addStretch(1);
    layout->addLayout(deleteRow);
    layout->addWidget(makeLabel(
        QStringLiteral("删除作用于全部匹配或不匹配的文件，不受搜索过滤影响。默认不会删除，需先预览再第二次确认。"),
        "hint"));

    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("filterUnique.search"));
    m_search->setPlaceholderText(QStringLiteral("过滤相对路径或匹配到的名单项"));
    m_search->setAccessibleName(QStringLiteral("搜索筛选结果"));
    m_search->setClearButtonEnabled(true);

    m_mode = new QComboBox;
    m_mode->setObjectName(QStringLiteral("filterUnique.resultMode"));
    m_mode->setAccessibleName(QStringLiteral("结果过滤"));
    m_mode->addItem(QStringLiteral("全部"));
    m_mode->addItem(QStringLiteral("仅匹配"));
    m_mode->addItem(QStringLiteral("仅不匹配"));

    m_visibleCount = makeLabel(QStringLiteral("当前显示 0 条"), "hint");
    m_visibleCount->setObjectName(QStringLiteral("filterUnique.visibleCount"));

    auto *filterRow = new QHBoxLayout;
    filterRow->setSpacing(8);
    filterRow->addWidget(makeLabel(QStringLiteral("搜索"), "fieldLabel"));
    filterRow->addWidget(m_search, 1);
    filterRow->addWidget(m_mode);
    filterRow->addWidget(m_visibleCount);
    layout->addLayout(filterRow);

    m_results = new QTableView;
    m_results->setObjectName(QStringLiteral("filterUnique.results"));
    m_results->setAccessibleName(QStringLiteral("筛选结果"));
    m_fileModel = new FilterUniqueFileModel(this);
    m_proxy = new FilterUniqueProxy(this);
    m_proxy->setSourceModel(m_fileModel);
    m_results->setModel(m_proxy);
    m_results->setSortingEnabled(true);
    m_results->sortByColumn(0, Qt::AscendingOrder);
    m_results->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_results->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_results->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_results->horizontalHeader()->setStretchLastSection(true);
    m_results->verticalHeader()->setVisible(false);
    m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results->setAlternatingRowColors(true);
    m_results->setWordWrap(false);
    m_results->setTextElideMode(Qt::ElideMiddle);
    m_results->setMinimumHeight(120);
    m_results->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_problems = new QTableView;
    m_problems->setObjectName(QStringLiteral("filterUnique.problems"));
    m_problems->setAccessibleName(QStringLiteral("问题列表"));
    m_problemModel = new FilterUniqueProblemModel(this);
    m_problems->setModel(m_problemModel);
    m_problems->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_problems->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_problems->horizontalHeader()->setStretchLastSection(true);
    m_problems->verticalHeader()->setVisible(false);
    m_problems->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_problems->setSelectionMode(QAbstractItemView::SingleSelection);
    m_problems->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_problems->setAlternatingRowColors(true);
    m_problems->setWordWrap(false);
    m_problems->setTextElideMode(Qt::ElideMiddle);
    m_problems->setMinimumHeight(80);
    m_problems->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *resultBox = new QWidget;
    auto *resultLayout = new QVBoxLayout(resultBox);
    resultLayout->setContentsMargins(0, 0, 0, 0);
    resultLayout->setSpacing(6);
    resultLayout->addWidget(makeLabel(QStringLiteral("筛选结果"), "sectionTitle"));
    resultLayout->addWidget(m_results, 1);

    auto *problemBox = new QWidget;
    auto *problemLayout = new QVBoxLayout(problemBox);
    problemLayout->setContentsMargins(0, 0, 0, 0);
    problemLayout->setSpacing(6);
    problemLayout->addWidget(makeLabel(QStringLiteral("问题"), "sectionTitle"));
    problemLayout->addWidget(m_problems, 1);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(resultBox);
    splitter->addWidget(problemBox);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    m_preview = new QFrame;
    m_preview->setObjectName(QStringLiteral("filterUnique.preview"));
    m_preview->setProperty("role", QStringLiteral("card"));
    auto *previewLayout = new QVBoxLayout(m_preview);
    previewLayout->setContentsMargins(16, 14, 16, 14);
    previewLayout->setSpacing(8);
    m_previewTitle = makeLabel(QStringLiteral("删除预览"), "sectionTitle");
    m_previewTitle->setObjectName(QStringLiteral("filterUnique.previewTitle"));
    m_previewHint = makeLabel(QString(), "hint");
    m_previewHint->setObjectName(QStringLiteral("filterUnique.previewHint"));
    m_previewList = new QListWidget;
    m_previewList->setObjectName(QStringLiteral("filterUnique.previewList"));
    m_previewList->setAccessibleName(QStringLiteral("即将删除的文件"));
    m_previewList->setMinimumHeight(80);
    m_previewList->setMaximumHeight(200);
    m_previewList->setUniformItemSizes(true);
    m_previewList->setTextElideMode(Qt::ElideMiddle);
    m_previewCancel = new QPushButton(QStringLiteral("取消"));
    m_previewCancel->setObjectName(QStringLiteral("filterUnique.previewCancel"));
    m_previewCancel->setAccessibleName(QStringLiteral("取消删除预览"));
    m_previewCancel->setAutoDefault(false);
    m_previewContinue = new QPushButton(QStringLiteral("名单无误，进入第二次确认"));
    m_previewContinue->setObjectName(QStringLiteral("filterUnique.previewContinue"));
    m_previewContinue->setAccessibleName(QStringLiteral("进入第二次确认"));
    m_previewContinue->setAutoDefault(false);
    m_previewBack = new QPushButton(QStringLiteral("返回核对"));
    m_previewBack->setObjectName(QStringLiteral("filterUnique.previewBack"));
    m_previewBack->setAccessibleName(QStringLiteral("返回删除预览"));
    m_previewBack->setAutoDefault(false);
    m_confirmDelete = new QPushButton(QStringLiteral("确认删除"));
    m_confirmDelete->setObjectName(QStringLiteral("filterUnique.confirmDelete"));
    m_confirmDelete->setProperty("role", QStringLiteral("danger"));
    m_confirmDelete->setAccessibleName(QStringLiteral("确认删除"));
    m_confirmDelete->setAutoDefault(false);
    auto *previewButtons = new QHBoxLayout;
    previewButtons->setSpacing(8);
    previewButtons->addWidget(m_previewCancel);
    previewButtons->addWidget(m_previewContinue);
    previewButtons->addWidget(m_previewBack);
    previewButtons->addWidget(m_confirmDelete);
    previewButtons->addStretch(1);
    previewLayout->addWidget(m_previewTitle);
    previewLayout->addWidget(m_previewHint);
    previewLayout->addWidget(m_previewList);
    previewLayout->addLayout(previewButtons);
    layout->addWidget(m_preview);

    connect(m_keywords, &QPlainTextEdit::textChanged, this, &FilterUniquePage::refreshKeywordCount);
    connect(m_import, &QPushButton::clicked, this, &FilterUniquePage::importFromDialog);
    connect(m_browse, &QPushButton::clicked, this, &FilterUniquePage::browseDirectory);
    connect(m_directory, &QLineEdit::textChanged, this, &FilterUniquePage::onDirectoryEdited);
    connect(m_directory, &QLineEdit::editingFinished, this, &FilterUniquePage::saveSession);
    connect(m_start, &QPushButton::clicked, this, &FilterUniquePage::startScan);
    connect(m_cancel, &QPushButton::clicked, this, &FilterUniquePage::cancelTask);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_proxy)
            m_proxy->setFilterFixedString(text);
        updateVisibleCount();
    });
    connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_proxy)
            m_proxy->setMode(index);
        updateVisibleCount();
    });
    connect(m_proxy, &QAbstractItemModel::layoutChanged, this, &FilterUniquePage::updateVisibleCount);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, &FilterUniquePage::updateVisibleCount);
    connect(m_deleteMatched, &QPushButton::clicked, this, [this]() { beginPreview(true); });
    connect(m_deleteUnmatched, &QPushButton::clicked, this, [this]() { beginPreview(false); });
    connect(m_previewCancel, &QPushButton::clicked, this, &FilterUniquePage::cancelPreview);
    connect(m_previewContinue, &QPushButton::clicked, this, &FilterUniquePage::continuePreview);
    connect(m_previewBack, &QPushButton::clicked, this, &FilterUniquePage::backToReview);
    connect(m_confirmDelete, &QPushButton::clicked, this, &FilterUniquePage::confirmDelete);

    setTabOrder(m_keywords, m_import);
    setTabOrder(m_import, m_directory);
    setTabOrder(m_directory, m_browse);
    setTabOrder(m_browse, m_start);
    setTabOrder(m_start, m_cancel);
    setTabOrder(m_cancel, m_deleteMatched);
    setTabOrder(m_deleteMatched, m_deleteUnmatched);
    setTabOrder(m_deleteUnmatched, m_search);
    setTabOrder(m_search, m_mode);
    setTabOrder(m_mode, m_results);
    setTabOrder(m_results, m_problems);
    setTabOrder(m_problems, m_previewList);
    setTabOrder(m_previewList, m_previewCancel);
    setTabOrder(m_previewCancel, m_previewContinue);
    setTabOrder(m_previewContinue, m_previewBack);
    setTabOrder(m_previewBack, m_confirmDelete);
}

void FilterUniquePage::loadSession()
{
    if (m_dataDir.isEmpty())
        return;
    QFile file(QDir(m_dataDir).filePath(QStringLiteral("session.json")));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return;
    const QJsonObject object = document.object();
    m_loading = true;
    m_keywords->setPlainText(object.value(QStringLiteral("keywords")).toString());
    m_directory->setText(object.value(QStringLiteral("directory")).toString());
    m_loading = false;
}

void FilterUniquePage::saveSession() const
{
    if (m_dataDir.isEmpty() || !m_keywords || !m_directory)
        return;
    const QString dirClean = QDir::cleanPath(m_dataDir);
    const QString path = QDir::cleanPath(QDir(dirClean).filePath(QStringLiteral("session.json")));
    if (!path.startsWith(dirClean + QLatin1Char('/')))
        return;

    QJsonObject object;
    object.insert(QStringLiteral("keywords"), m_keywords->toPlainText());
    object.insert(QStringLiteral("directory"), m_directory->text());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        m_services.logError(QStringLiteral("无法保存本地状态：%1").arg(file.errorString()));
        return;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    if (!file.commit())
        m_services.logError(QStringLiteral("无法保存本地状态：%1").arg(file.errorString()));
}

void FilterUniquePage::refreshKeywordCount()
{
    if (!m_keywords || !m_keywordCount)
        return;
    const int count = parseKeywords(m_keywords->toPlainText()).size();
    m_keywordCount->setText(QStringLiteral("已识别 %1 个关键词").arg(count));
}

void FilterUniquePage::startScan()
{
    if (m_busy || m_aborting)
        return;
    saveSession();

    auto reject = [this](const QString &message) {
        m_services.logError(message);
        m_services.showStatus(StatusState::Error, message);
        if (m_detail)
            m_detail->setText(message);
    };

    const QStringList keywords = parseKeywords(m_keywords->toPlainText());
    refreshKeywordCount();
    if (keywords.isEmpty()) {
        reject(QStringLiteral("请填写筛选名单，每行一个名称或关键词"));
        return;
    }

    const QString typed = m_directory->text().trimmed();
    if (typed.isEmpty()) {
        reject(QStringLiteral("请填写要筛选的目录"));
        return;
    }
    const QString clean = QDir::cleanPath(typed);
    const QFileInfo info(clean);
    if (info.isRelative()) {
        reject(QStringLiteral("目录必须是绝对路径"));
        return;
    }
    if (clean == QLatin1String("/") || info.canonicalFilePath() == QLatin1String("/")) {
        reject(QStringLiteral("不能选择文件系统根目录"));
        return;
    }
    if (!info.exists()) {
        reject(QStringLiteral("目录不存在：%1").arg(clean));
        return;
    }
    if (!info.isDir()) {
        reject(QStringLiteral("路径不是目录：%1").arg(clean));
        return;
    }
    if (info.canonicalFilePath().isEmpty()) {
        reject(QStringLiteral("无法读取该目录：%1").arg(clean));
        return;
    }

    clearResults();
    auto job = std::make_shared<FilterJob>();
    job->kind = FilterJob::Kind::Scan;
    job->root = clean;
    job->keywords = keywords;
    job->stepDelayMs = m_stepDelayMs;
    FilterUniquePage *page = this;
    job->progress = [page](int files, int problems) {
        const QString text = QStringLiteral("已扫描 %1 个文件，问题 %2 个").arg(files).arg(problems);
        QMetaObject::invokeMethod(page, "setBusyDetail", Qt::QueuedConnection, Q_ARG(QString, text));
    };
    m_progress->setRange(0, 0);
    m_detail->setText(QStringLiteral("正在筛选…"));
    startJob(job, QStringLiteral("正在筛选文件"),
             QStringLiteral("正在筛选文件（关键词 %1 个）").arg(keywords.size()));
}

void FilterUniquePage::cancelTask()
{
    if (!m_busy || !m_job)
        return;
    m_job->cancel.store(true);
    m_cancel->setEnabled(false);
    m_detail->setText(QStringLiteral("正在取消…"));
    m_services.showStatus(StatusState::Busy, QStringLiteral("正在取消…"));
    m_services.logWarning(QStringLiteral("正在取消"));
}

void FilterUniquePage::browseDirectory()
{
    const QString current = m_directory->text().trimmed();
    const QString start = QFileInfo(current).isDir() ? current : QDir::homePath();
    const QString selected = QFileDialog::getExistingDirectory(
        m_dialogParent, QStringLiteral("选择要筛选的目录"), start,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty())
        return;
    m_directory->setText(QDir::cleanPath(selected));
    saveSession();
}

void FilterUniquePage::importFromDialog()
{
    const QString path = QFileDialog::getOpenFileName(
        m_dialogParent, QStringLiteral("导入筛选名单"), QString(),
        QStringLiteral("文本文件 (*.txt *.list *.csv);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!importKeywordsFromFile(path, &error)) {
        m_services.logError(error);
        m_services.showStatus(StatusState::Error, error);
    }
}

void FilterUniquePage::onDirectoryEdited(const QString &text)
{
    if (m_loading || m_busy || m_scannedRoot.isEmpty())
        return;
    if (QDir::cleanPath(text.trimmed()) != m_scannedRoot)
        clearResults();
}

void FilterUniquePage::updateVisibleCount()
{
    if (!m_visibleCount)
        return;
    const int shown = m_proxy ? m_proxy->rowCount() : 0;
    m_visibleCount->setText(QStringLiteral("当前显示 %1 条").arg(shown));
}

void FilterUniquePage::recount()
{
    m_total = m_fileModel ? m_fileModel->rows().size() : 0;
    m_matched = 0;
    if (m_fileModel) {
        for (const FilterFileRow &row : m_fileModel->rows()) {
            if (row.matched)
                ++m_matched;
        }
    }
    m_unmatched = m_total - m_matched;
    m_problemCount = m_problemModel ? m_problemModel->problems().size() : 0;
    if (m_matchedLabel)
        m_matchedLabel->setText(QString::number(m_matched));
    if (m_unmatchedLabel)
        m_unmatchedLabel->setText(QString::number(m_unmatched));
    if (m_totalLabel)
        m_totalLabel->setText(QString::number(m_total));
    if (m_problemLabel)
        m_problemLabel->setText(QString::number(m_problemCount));
    updateVisibleCount();
}

void FilterUniquePage::updateUi()
{
    const bool busy = m_busy;
    const bool previewOpen = m_previewStage != PreviewStage::Hidden;
    if (m_start)
        m_start->setEnabled(!busy && !m_aborting && !previewOpen);
    if (m_cancel)
        m_cancel->setEnabled(busy);
    if (m_deleteMatched) {
        m_deleteMatched->setEnabled(!busy && !previewOpen && m_matched > 0);
        m_deleteMatched->setText(QStringLiteral("删除匹配的文件（%1）").arg(m_matched));
        m_deleteMatched->setAccessibleName(m_deleteMatched->text());
    }
    if (m_deleteUnmatched) {
        m_deleteUnmatched->setEnabled(!busy && !previewOpen && m_unmatched > 0);
        m_deleteUnmatched->setText(QStringLiteral("删除不匹配的文件（%1）").arg(m_unmatched));
        m_deleteUnmatched->setAccessibleName(m_deleteUnmatched->text());
    }
    if (m_keywords)
        m_keywords->setReadOnly(busy);
    if (m_directory)
        m_directory->setReadOnly(busy);
    if (m_import)
        m_import->setEnabled(!busy);
    if (m_browse)
        m_browse->setEnabled(!busy);

    const bool review = m_previewStage == PreviewStage::Review;
    const bool confirm = m_previewStage == PreviewStage::Confirm;
    if (m_preview)
        m_preview->setVisible(previewOpen);
    if (m_previewContinue) {
        m_previewContinue->setVisible(review);
        m_previewContinue->setEnabled(review && !busy);
    }
    if (m_previewBack) {
        m_previewBack->setVisible(confirm);
        m_previewBack->setEnabled(confirm && !busy);
    }
    if (m_confirmDelete) {
        m_confirmDelete->setVisible(confirm);
        m_confirmDelete->setEnabled(confirm && !busy && !m_pendingPaths.isEmpty());
    }
    if (m_previewCancel)
        m_previewCancel->setEnabled(previewOpen && !busy);
}

void FilterUniquePage::beginPreview(bool matched)
{
    if (m_busy || m_scannedRoot.isEmpty())
        return;
    const QStringList paths = pathsFor(matched);
    if (paths.isEmpty()) {
        const QString message = QStringLiteral("没有可删除的文件");
        m_services.showStatus(StatusState::Info, message);
        return;
    }
    m_previewMatched = matched;
    m_pendingPaths = paths;
    m_previewList->clear();
    for (const QString &path : paths) {
        const QString relative = QDir(m_scannedRoot).relativeFilePath(path);
        auto *item = new QListWidgetItem(QDir::fromNativeSeparators(relative));
        item->setToolTip(path);
        item->setData(Qt::UserRole, path);
        m_previewList->addItem(item);
    }
    setPreviewStage(PreviewStage::Review);
    m_services.logInfo(QStringLiteral("已生成删除预览，共 %1 个文件，尚未删除").arg(paths.size()));
}

void FilterUniquePage::continuePreview()
{
    if (m_previewStage != PreviewStage::Review || m_busy)
        return;
    setPreviewStage(PreviewStage::Confirm);
}

void FilterUniquePage::backToReview()
{
    if (m_previewStage != PreviewStage::Confirm || m_busy)
        return;
    setPreviewStage(PreviewStage::Review);
}

void FilterUniquePage::cancelPreview()
{
    if (m_busy)
        return;
    setPreviewStage(PreviewStage::Hidden);
    m_services.showStatus(StatusState::Info, QStringLiteral("已取消删除，未改动目录"));
}

void FilterUniquePage::confirmDelete()
{
    if (m_previewStage != PreviewStage::Confirm || m_busy || m_pendingPaths.isEmpty() || m_scannedRoot.isEmpty())
        return;
    const QStringList paths = m_pendingPaths;
    const QString root = m_scannedRoot;
    setPreviewStage(PreviewStage::Hidden);

    auto job = std::make_shared<FilterJob>();
    job->kind = FilterJob::Kind::Delete;
    job->root = root;
    job->paths = paths;
    job->stepDelayMs = m_stepDelayMs;
    FilterUniquePage *page = this;
    job->progress = [page](int done, int total) {
        const QString text = QStringLiteral("已处理 %1 / %2").arg(done).arg(total);
        QMetaObject::invokeMethod(page, "setBusyDetail", Qt::QueuedConnection, Q_ARG(QString, text));
    };
    m_progress->setRange(0, 0);
    m_detail->setText(QStringLiteral("正在删除…"));
    startJob(job, QStringLiteral("正在删除文件"), QStringLiteral("正在删除文件"));
}

void FilterUniquePage::startJob(const std::shared_ptr<FilterJob> &job, const QString &lockReason,
                                const QString &statusText)
{
    if (m_busy || m_aborting || !job)
        return;
    m_job = job;
    m_busy = true;
    m_lock = m_services.lockNavigation(lockReason);
    m_services.showStatus(StatusState::Busy, statusText);
    m_services.logInfo(statusText);
    m_thread = new FilterUniqueThread(job);
    connect(m_thread, &QThread::finished, this, &FilterUniquePage::onThreadFinished, Qt::QueuedConnection);
    updateUi();
    m_thread->start();
}

void FilterUniquePage::onThreadFinished()
{
    const bool aborting = m_aborting;
    std::shared_ptr<FilterJob> job = std::move(m_job);
    if (m_thread) {
        QThread *thread = m_thread;
        m_thread = nullptr;
        thread->deleteLater();
    }
    m_busy = false;
    m_lock.release();
    if (!aborting && job) {
        if (job->kind == FilterJob::Kind::Scan)
            applyScan(job->scanResult);
        else
            applyDelete(job->deleteResult);
    }
    if (!aborting)
        updateUi();
}

void FilterUniquePage::applyScan(const FilterScanResult &result)
{
    if (result.cancelled) {
        clearResults();
        m_progress->setRange(0, 100);
        m_progress->setValue(0);
        m_detail->setText(QStringLiteral("已取消"));
        m_services.logWarning(QStringLiteral("筛选已取消，未改动目录"));
        m_services.showStatus(StatusState::Info, QStringLiteral("已取消筛选，未改动目录"));
        return;
    }
    if (!result.error.isEmpty()) {
        clearResults();
        m_progress->setRange(0, 100);
        m_progress->setValue(0);
        m_detail->setText(result.error);
        m_services.logError(result.error);
        m_services.showStatus(StatusState::Error, result.error);
        return;
    }

    m_fileModel->setRows(result.files);
    m_problemModel->setProblems(result.problems);
    m_scannedRoot = result.rootPath;
    recount();
    m_progress->setRange(0, 100);
    m_progress->setValue(100);
    const QString summary = QStringLiteral("筛选完成：匹配 %1，不匹配 %2，共 %3")
                                .arg(m_matched)
                                .arg(m_unmatched)
                                .arg(m_total);
    m_detail->setText(summary);
    m_services.logSuccess(summary);
    if (!result.problems.isEmpty()) {
        m_services.logWarning(QStringLiteral("有 %1 项无法读取或已跳过").arg(result.problems.size()));
        const int limit = qMin(result.problems.size(), 5);
        for (int i = 0; i < limit; ++i) {
            m_services.logWarning(QStringLiteral("%1：%2")
                                      .arg(result.problems.at(i).path, result.problems.at(i).reason));
        }
    }
    m_services.showStatus(StatusState::Success, summary);
}

void FilterUniquePage::applyDelete(const FilterDeleteResult &result)
{
    int removed = 0;
    QVector<FilterProblem> failures;
    QSet<QString> removedPaths;
    for (const FilterDeleteItem &item : result.items) {
        if (item.removed) {
            ++removed;
            removedPaths.insert(QDir::cleanPath(item.absolutePath));
        } else if (!item.error.isEmpty()) {
            failures.append(FilterProblem{item.absolutePath, item.error});
        }
    }

    if (m_fileModel) {
        QVector<FilterFileRow> remain;
        remain.reserve(m_fileModel->rows().size());
        for (const FilterFileRow &row : m_fileModel->rows()) {
            if (!removedPaths.contains(QDir::cleanPath(row.absolutePath)))
                remain.append(row);
        }
        m_fileModel->setRows(remain);
    }
    if (m_problemModel && !failures.isEmpty())
        m_problemModel->addProblems(failures);
    recount();
    m_progress->setRange(0, 100);
    m_progress->setValue(result.cancelled ? 0 : 100);

    if (!result.error.isEmpty()) {
        m_detail->setText(result.error);
        m_services.logError(result.error);
        m_services.showStatus(StatusState::Error, result.error);
        return;
    }
    if (result.cancelled) {
        const QString text = QStringLiteral("已取消删除：已删除 %1 个，其余未改动").arg(removed);
        m_detail->setText(text);
        m_services.logWarning(text);
        m_services.showStatus(StatusState::Info, text);
        return;
    }
    if (!failures.isEmpty()) {
        const QString text = QStringLiteral("已删除 %1 个，失败 %2 个").arg(removed).arg(failures.size());
        m_detail->setText(text);
        m_services.logError(text);
        const int limit = qMin(failures.size(), 5);
        for (int i = 0; i < limit; ++i)
            m_services.logError(QStringLiteral("%1：%2").arg(failures.at(i).path, failures.at(i).reason));
        m_services.showStatus(StatusState::Error, text);
        return;
    }
    const QString text = QStringLiteral("已删除 %1 个文件").arg(removed);
    m_detail->setText(text);
    m_services.logSuccess(text);
    m_services.showStatus(StatusState::Success, text);
}

void FilterUniquePage::clearResults()
{
    if (m_fileModel)
        m_fileModel->setRows({});
    if (m_problemModel)
        m_problemModel->setProblems({});
    m_scannedRoot.clear();
    m_pendingPaths.clear();
    if (m_previewList)
        m_previewList->clear();
    m_previewStage = PreviewStage::Hidden;
    recount();
    updateUi();
}

void FilterUniquePage::setPreviewStage(PreviewStage stage)
{
    m_previewStage = stage;
    if (stage == PreviewStage::Hidden)
        m_pendingPaths.clear();
    else if (stage == PreviewStage::Review) {
        m_previewTitle->setText(m_previewMatched ? QStringLiteral("第一次确认：将删除匹配的文件")
                                                 : QStringLiteral("第一次确认：将删除不匹配的文件"));
        m_previewHint->setText(
            QStringLiteral("请核对下面的 %1 个文件。此时尚未删除任何文件，所选目录也不会被改动。")
                .arg(m_pendingPaths.size()));
    } else {
        m_previewTitle->setText(QStringLiteral("第二次确认"));
        m_previewHint->setText(
            QStringLiteral("再次确认后才会删除上面列出的 %1 个文件，此操作不可恢复。不会删除所选目录，也不会删除任何子目录。")
                .arg(m_pendingPaths.size()));
    }
    updateUi();
}

QStringList FilterUniquePage::pathsFor(bool matched) const
{
    QStringList paths;
    if (!m_fileModel)
        return paths;
    for (const FilterFileRow &row : m_fileModel->rows()) {
        if (row.matched == matched)
            paths.append(row.absolutePath);
    }
    return paths;
}

void FilterUniquePage::setBusyDetail(const QString &text)
{
    if (m_aborting || !m_busy || !m_detail)
        return;
    m_detail->setText(text);
}

ToolInfo FilterUniqueTool::info() const
{
    ToolInfo info;
    info.id = QStringLiteral("filter-unique");
    info.title = QStringLiteral("筛选去重");
    info.summary = QStringLiteral("按名单模糊筛选目录中的文件，确认后可删除");
    info.iconKey = QStringLiteral("task");
    info.order = 40;
    return info;
}

bool FilterUniqueTool::initialize(const ToolContext &context, QString *error)
{
    m_context = context;
    QString dirError;
    m_dataDir = m_context.services.dataDir(&dirError);
    if (m_dataDir.isEmpty()) {
        if (error)
            *error = dirError.isEmpty() ? QStringLiteral("无法创建工具数据目录") : dirError;
        return false;
    }
    return true;
}

QVector<ToolPage> FilterUniqueTool::createPages()
{
    auto *page = new FilterUniquePage(m_context.services, m_dataDir, m_context.dialogParent);
    m_page = page;

    QVector<ToolPage> pages;
    pages.append({PageInfo{QStringLiteral("filter"), QStringLiteral("筛选"),
                           QStringLiteral("按名单筛选并确认后删除"), QStringLiteral("task")},
                  page});
    return pages;
}

void FilterUniqueTool::shutdown()
{
    if (m_page)
        m_page->abort();
}

} // namespace tw
