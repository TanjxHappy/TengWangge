#include "tools/filter_unique_high/high_filter_page.h"

#include "shell/elided_label.h"
#include "tools/filter_unique_high/bounded_tree.h"
#include "tools/filter_unique_high/term_list.h"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
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
#include <QSet>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTableView>
#include <QTextCodec>
#include <QThread>
#include <QVBoxLayout>

#include <atomic>

namespace tw {
namespace {

QLabel *makeLabel(const QString &text, const char *role)
{
    auto *label = new QLabel(text);
    label->setProperty("role", QString::fromLatin1(role));
    return label;
}

} // namespace

class HitModel : public QAbstractTableModel
{
public:
    explicit HitModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : m_files.size();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : 3;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_files.size())
            return {};
        const ListedFile &file = m_files.at(index.row());
        if (role == Qt::ToolTipRole && index.column() == 0)
            return file.absolutePath;
        if (role != Qt::DisplayRole)
            return {};
        switch (index.column()) {
        case 0:
            return file.relativePath;
        case 1:
            return file.matched ? QStringLiteral("匹配") : QStringLiteral("不匹配");
        case 2:
            return file.hits.isEmpty() ? QStringLiteral("无")
                                       : file.hits.join(QStringLiteral("、"));
        default:
            return {};
        }
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
            return QStringLiteral("命中的名单项");
        default:
            return {};
        }
    }

    void setFiles(const QVector<ListedFile> &files)
    {
        beginResetModel();
        m_files = files;
        endResetModel();
    }

    void removePaths(const QStringList &paths)
    {
        if (paths.isEmpty())
            return;
        QSet<QString> gone;
        for (const QString &path : paths)
            gone.insert(path);
        QVector<ListedFile> kept;
        kept.reserve(m_files.size());
        for (const ListedFile &file : m_files) {
            if (!gone.contains(file.absolutePath))
                kept.append(file);
        }
        setFiles(kept);
    }

    QStringList pathsWhere(bool matched) const
    {
        QStringList paths;
        for (const ListedFile &file : m_files) {
            if (file.matched == matched)
                paths.append(file.absolutePath);
        }
        return paths;
    }

    const ListedFile *fileAt(int row) const
    {
        if (row < 0 || row >= m_files.size())
            return nullptr;
        return &m_files.at(row);
    }

private:
    QVector<ListedFile> m_files;
};

class IssueModel : public QAbstractTableModel
{
public:
    explicit IssueModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : m_issues.size();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : 2;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_issues.size())
            return {};
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
            return {};
        const TreeIssue &issue = m_issues.at(index.row());
        if (index.column() == 0)
            return issue.path;
        if (index.column() == 1)
            return issue.reason;
        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        if (section == 0)
            return QStringLiteral("路径");
        if (section == 1)
            return QStringLiteral("原因");
        return {};
    }

    void setIssues(const QVector<TreeIssue> &issues)
    {
        beginResetModel();
        m_issues = issues;
        endResetModel();
    }

    void appendIssue(const TreeIssue &issue)
    {
        const int row = m_issues.size();
        beginInsertRows(QModelIndex(), row, row);
        m_issues.append(issue);
        endInsertRows();
    }

private:
    QVector<TreeIssue> m_issues;
};

class HitProxy : public QSortFilterProxyModel
{
public:
    explicit HitProxy(HitModel *model, QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
        , m_model(model)
    {
        setSourceModel(model);
        setDynamicSortFilter(false);
    }

    void setNeedle(const QString &text)
    {
        m_needle = text.toCaseFolded();
        invalidateFilter();
    }

    void setMode(int mode)
    {
        m_mode = mode;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &) const override
    {
        const ListedFile *file = m_model->fileAt(sourceRow);
        if (!file)
            return false;
        if (m_mode == 1 && !file->matched)
            return false;
        if (m_mode == 2 && file->matched)
            return false;
        if (m_needle.isEmpty())
            return true;
        if (file->relativePath.toCaseFolded().contains(m_needle)
            || file->fileName.toCaseFolded().contains(m_needle))
            return true;
        for (const QString &hit : file->hits) {
            if (hit.toCaseFolded().contains(m_needle))
                return true;
        }
        return false;
    }

private:
    HitModel *m_model = nullptr;
    QString m_needle;
    int m_mode = 0;
};

QString stateFilePath(const QString &dataDir)
{
    return QDir(dataDir).filePath(QStringLiteral("high-filter-state.json"));
}

struct Job
{
    enum class Kind { Walk, Remove };

    Kind kind = Kind::Walk;
    std::atomic<bool> cancel{false};
    int paceMs = 0;
    QString root;
    QVector<Term> terms;
    QStringList paths;
    TreeWalk walk;
    RemovalBatch removal;
};

HighFilterPage::HighFilterPage(const ToolServices &services, const QString &dataDir,
                               QWidget *dialogParent, QWidget *parent)
    : QWidget(parent)
    , m_services(services)
    , m_dataDir(dataDir)
    , m_dialogParent(dialogParent)
{
    buildUi();
}

HighFilterPage::~HighFilterPage()
{
    abort();
}

void HighFilterPage::abort()
{
    m_aborting = true;
    if (m_job)
        m_job->cancel.store(true);
    if (m_thread) {
        m_thread->wait();
        disconnect(m_thread, nullptr, this, nullptr);
        QCoreApplication::removePostedEvents(this);
        delete m_thread;
        m_thread = nullptr;
    }
    m_job.reset();
    m_busy = false;
    m_lock.release();
}

bool HighFilterPage::loadTermsFromFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取名单：%1").arg(file.errorString());
        return false;
    }
    QByteArray bytes = file.readAll();
    if (bytes.startsWith("\xEF\xBB\xBF"))
        bytes.remove(0, 3);

    QTextCodec *utf8 = QTextCodec::codecForName("UTF-8");
    if (!utf8) {
        if (error)
            *error = QStringLiteral("当前环境无法解析 UTF-8");
        return false;
    }
    QTextCodec::ConverterState state;
    const QString text = utf8->toUnicode(bytes.constData(), bytes.size(), &state);
    if (state.invalidChars > 0) {
        if (error)
            *error = QStringLiteral("文件不是有效的 UTF-8 文本");
        return false;
    }
    m_terms->setPlainText(text);
    return true;
}

void HighFilterPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 8, 12, 8);
    root->setSpacing(6);

    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(12);
    titleRow->addWidget(makeLabel(QStringLiteral("筛选去重high"), "pageTitle"));
    m_rule = new ElidedLabel;
    m_rule->setObjectName(QStringLiteral("highFilter.rule"));
    m_rule->setProperty("role", QStringLiteral("hint"));
    m_rule->setAccessibleName(QStringLiteral("筛选规则"));
    m_rule->setFullText(QStringLiteral(
        "不区分大小写的子串匹配相对路径和文件名，不读内容。符号链接不越界、不循环。"
        "删除前预览完整列表并第二次确认，未确认不改目录。"));
    titleRow->addWidget(m_rule, 1);
    root->addLayout(titleRow);

    m_terms = new QPlainTextEdit;
    m_terms->setObjectName(QStringLiteral("highFilter.terms"));
    m_terms->setFixedHeight(64);
    m_terms->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_terms->setTabChangesFocus(true);
    m_terms->setPlaceholderText(QStringLiteral("每行一个关键词，空行忽略，重复项按大小写不敏感合并"));
    m_terms->setAccessibleName(QStringLiteral("关键词名单"));
    m_import = new QPushButton(QStringLiteral("导入名单"));
    m_import->setObjectName(QStringLiteral("highFilter.import"));
    m_import->setAccessibleName(QStringLiteral("导入名单"));
    m_termCount = makeLabel(QStringLiteral("已识别 0 个关键词"), "hint");
    m_termCount->setObjectName(QStringLiteral("highFilter.termCount"));

    auto *termRow = new QHBoxLayout;
    termRow->setSpacing(8);
    termRow->addWidget(m_terms, 1);
    auto *termSide = new QVBoxLayout;
    termSide->setSpacing(4);
    termSide->addWidget(m_import);
    termSide->addWidget(m_termCount);
    termSide->addStretch(1);
    termRow->addLayout(termSide);
    root->addLayout(termRow);

    m_directory = new QLineEdit;
    m_directory->setObjectName(QStringLiteral("highFilter.directory"));
    m_directory->setPlaceholderText(QStringLiteral("绝对路径目录，不能是 /"));
    m_directory->setAccessibleName(QStringLiteral("要筛选的目录"));
    m_browse = new QPushButton(QStringLiteral("浏览…"));
    m_browse->setObjectName(QStringLiteral("highFilter.browse"));
    m_browse->setAccessibleName(QStringLiteral("浏览目录"));
    auto *dirRow = new QHBoxLayout;
    dirRow->setSpacing(8);
    dirRow->addWidget(makeLabel(QStringLiteral("目录"), "fieldLabel"));
    dirRow->addWidget(m_directory, 1);
    dirRow->addWidget(m_browse);
    root->addLayout(dirRow);

    m_start = new QPushButton(QStringLiteral("开始筛选"));
    m_start->setObjectName(QStringLiteral("highFilter.start"));
    m_start->setProperty("role", QStringLiteral("primary"));
    m_start->setAccessibleName(QStringLiteral("开始筛选"));
    m_cancel = new QPushButton(QStringLiteral("取消"));
    m_cancel->setObjectName(QStringLiteral("highFilter.cancel"));
    m_cancel->setAccessibleName(QStringLiteral("取消当前任务"));
    m_progress = new QProgressBar;
    m_progress->setObjectName(QStringLiteral("highFilter.progress"));
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setAccessibleName(QStringLiteral("筛选进度"));
    m_detail = makeLabel(QStringLiteral("尚未筛选"), "hint");
    m_detail->setObjectName(QStringLiteral("highFilter.detail"));
    m_detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_detail->setMinimumWidth(0);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addWidget(m_start);
    actions->addWidget(m_cancel);
    actions->addWidget(m_progress, 1);
    actions->addWidget(m_detail, 2);
    root->addLayout(actions);

    m_matchedCount = makeLabel(QStringLiteral("0"), "value");
    m_matchedCount->setObjectName(QStringLiteral("highFilter.matchedCount"));
    m_unmatchedCount = makeLabel(QStringLiteral("0"), "value");
    m_unmatchedCount->setObjectName(QStringLiteral("highFilter.unmatchedCount"));
    m_totalCount = makeLabel(QStringLiteral("0"), "value");
    m_totalCount->setObjectName(QStringLiteral("highFilter.totalCount"));
    m_issueLabel = makeLabel(QStringLiteral("0"), "value");
    m_issueLabel->setObjectName(QStringLiteral("highFilter.issueCount"));
    m_deleteMatched = new QPushButton(QStringLiteral("删除匹配文件"));
    m_deleteMatched->setObjectName(QStringLiteral("highFilter.deleteMatched"));
    m_deleteMatched->setProperty("role", QStringLiteral("danger"));
    m_deleteMatched->setAccessibleName(QStringLiteral("删除匹配文件"));
    m_deleteUnmatched = new QPushButton(QStringLiteral("删除不匹配文件"));
    m_deleteUnmatched->setObjectName(QStringLiteral("highFilter.deleteUnmatched"));
    m_deleteUnmatched->setProperty("role", QStringLiteral("danger"));
    m_deleteUnmatched->setAccessibleName(QStringLiteral("删除不匹配文件"));
    auto *stats = new QHBoxLayout;
    stats->setSpacing(8);
    stats->addWidget(makeLabel(QStringLiteral("匹配"), "fieldLabel"));
    stats->addWidget(m_matchedCount);
    stats->addSpacing(8);
    stats->addWidget(makeLabel(QStringLiteral("不匹配"), "fieldLabel"));
    stats->addWidget(m_unmatchedCount);
    stats->addSpacing(8);
    stats->addWidget(makeLabel(QStringLiteral("总数"), "fieldLabel"));
    stats->addWidget(m_totalCount);
    stats->addSpacing(8);
    stats->addWidget(makeLabel(QStringLiteral("问题"), "fieldLabel"));
    stats->addWidget(m_issueLabel);
    stats->addStretch(1);
    stats->addWidget(m_deleteMatched);
    stats->addWidget(m_deleteUnmatched);
    root->addLayout(stats);

    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("highFilter.search"));
    m_search->setPlaceholderText(QStringLiteral("搜索路径或命中项"));
    m_search->setAccessibleName(QStringLiteral("搜索结果"));
    m_search->setClearButtonEnabled(true);
    m_viewMode = new QComboBox;
    m_viewMode->setObjectName(QStringLiteral("highFilter.viewMode"));
    m_viewMode->setAccessibleName(QStringLiteral("结果显示范围"));
    m_viewMode->addItem(QStringLiteral("全部"));
    m_viewMode->addItem(QStringLiteral("仅匹配"));
    m_viewMode->addItem(QStringLiteral("仅不匹配"));
    m_visibleCount = makeLabel(QStringLiteral("可见 0"), "hint");
    m_visibleCount->setObjectName(QStringLiteral("highFilter.visibleCount"));
    auto *searchRow = new QHBoxLayout;
    searchRow->setSpacing(8);
    searchRow->addWidget(makeLabel(QStringLiteral("搜索"), "fieldLabel"));
    searchRow->addWidget(m_search, 1);
    searchRow->addWidget(makeLabel(QStringLiteral("显示"), "fieldLabel"));
    searchRow->addWidget(m_viewMode);
    searchRow->addWidget(m_visibleCount);
    root->addLayout(searchRow);

    m_files = new HitModel(this);
    m_proxy = new HitProxy(m_files, this);
    m_results = new QTableView;
    m_results->setObjectName(QStringLiteral("highFilter.results"));
    m_results->setModel(m_proxy);
    m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results->setShowGrid(false);
    m_results->setWordWrap(false);
    m_results->setTextElideMode(Qt::ElideMiddle);
    m_results->verticalHeader()->setVisible(false);
    m_results->horizontalHeader()->setStretchLastSection(false);
    m_results->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_results->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_results->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_results->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_results->setMinimumHeight(120);
    m_results->setAccessibleName(QStringLiteral("筛选结果"));
    root->addWidget(m_results, 1);

    m_issueModel = new IssueModel(this);
    m_issues = new QTableView;
    m_issues->setObjectName(QStringLiteral("highFilter.issues"));
    m_issues->setModel(m_issueModel);
    m_issues->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_issues->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_issues->setShowGrid(false);
    m_issues->setWordWrap(false);
    m_issues->verticalHeader()->setVisible(false);
    m_issues->horizontalHeader()->setStretchLastSection(true);
    m_issues->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_issues->setAccessibleName(QStringLiteral("问题列表"));

    m_preview = new QWidget;
    m_preview->setObjectName(QStringLiteral("highFilter.preview"));
    auto *previewLayout = new QVBoxLayout(m_preview);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(4);
    m_previewTitle = makeLabel(QStringLiteral("预览"), "sectionTitle");
    m_previewTitle->setObjectName(QStringLiteral("highFilter.previewTitle"));
    m_previewHint = makeLabel(QString(), "hint");
    m_previewHint->setObjectName(QStringLiteral("highFilter.previewHint"));
    m_previewList = new QListWidget;
    m_previewList->setObjectName(QStringLiteral("highFilter.previewList"));
    m_previewList->setAccessibleName(QStringLiteral("待删除文件完整列表"));
    m_previewCancel = new QPushButton(QStringLiteral("取消"));
    m_previewCancel->setObjectName(QStringLiteral("highFilter.previewCancel"));
    m_previewCancel->setAccessibleName(QStringLiteral("取消删除预览"));
    m_previewBack = new QPushButton(QStringLiteral("返回"));
    m_previewBack->setObjectName(QStringLiteral("highFilter.previewBack"));
    m_previewBack->setAccessibleName(QStringLiteral("返回预览"));
    m_previewContinue = new QPushButton(QStringLiteral("继续"));
    m_previewContinue->setObjectName(QStringLiteral("highFilter.previewContinue"));
    m_previewContinue->setProperty("role", QStringLiteral("primary"));
    m_previewContinue->setAccessibleName(QStringLiteral("继续到第二次确认"));
    m_confirmDelete = new QPushButton(QStringLiteral("确认删除"));
    m_confirmDelete->setObjectName(QStringLiteral("highFilter.confirmDelete"));
    m_confirmDelete->setProperty("role", QStringLiteral("danger"));
    m_confirmDelete->setAccessibleName(QStringLiteral("确认删除"));
    m_confirmDelete->setEnabled(false);
    auto *previewButtons = new QHBoxLayout;
    previewButtons->setSpacing(8);
    previewButtons->addWidget(m_previewCancel);
    previewButtons->addWidget(m_previewBack);
    previewButtons->addWidget(m_previewContinue);
    previewButtons->addWidget(m_confirmDelete);
    previewButtons->addStretch(1);
    previewLayout->addWidget(m_previewTitle);
    previewLayout->addWidget(m_previewHint);
    previewLayout->addWidget(m_previewList, 1);
    previewLayout->addLayout(previewButtons);

    auto *issuesPage = new QWidget;
    auto *issuesLayout = new QVBoxLayout(issuesPage);
    issuesLayout->setContentsMargins(0, 0, 0, 0);
    issuesLayout->setSpacing(0);
    issuesLayout->addWidget(m_issues);

    m_stack = new QStackedWidget;
    m_stack->setObjectName(QStringLiteral("highFilter.bottom"));
    m_stack->setFixedHeight(124);
    m_stack->addWidget(issuesPage);
    m_stack->addWidget(m_preview);
    root->addWidget(m_stack);

    loadState();
    refreshTermCount();

    connect(m_terms, &QPlainTextEdit::textChanged, this, &HighFilterPage::onTermsEdited);
    connect(m_directory, &QLineEdit::textChanged, this, &HighFilterPage::onDirectoryEdited);
    connect(m_import, &QPushButton::clicked, this, &HighFilterPage::importFromDialog);
    connect(m_browse, &QPushButton::clicked, this, &HighFilterPage::browseDirectory);
    connect(m_start, &QPushButton::clicked, this, &HighFilterPage::startScan);
    connect(m_cancel, &QPushButton::clicked, this, &HighFilterPage::cancelTask);
    connect(m_search, &QLineEdit::textChanged, this, &HighFilterPage::onSearchChanged);
    connect(m_viewMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &HighFilterPage::onModeChanged);
    connect(m_deleteMatched, &QPushButton::clicked, this, [this]() { beginPreview(true); });
    connect(m_deleteUnmatched, &QPushButton::clicked, this, [this]() { beginPreview(false); });
    connect(m_previewCancel, &QPushButton::clicked, this, &HighFilterPage::cancelPreview);
    connect(m_previewBack, &QPushButton::clicked, this, &HighFilterPage::backToReview);
    connect(m_previewContinue, &QPushButton::clicked, this, &HighFilterPage::continuePreview);
    connect(m_confirmDelete, &QPushButton::clicked, this, &HighFilterPage::confirmDelete);

    setTabOrder(m_terms, m_import);
    setTabOrder(m_import, m_directory);
    setTabOrder(m_directory, m_browse);
    setTabOrder(m_browse, m_start);
    setTabOrder(m_start, m_cancel);
    setTabOrder(m_cancel, m_search);
    setTabOrder(m_search, m_viewMode);
    setTabOrder(m_viewMode, m_results);
    setTabOrder(m_results, m_issues);
    setTabOrder(m_issues, m_deleteMatched);
    setTabOrder(m_deleteMatched, m_deleteUnmatched);
    setTabOrder(m_deleteUnmatched, m_previewList);
    setTabOrder(m_previewList, m_previewCancel);
    setTabOrder(m_previewCancel, m_previewBack);
    setTabOrder(m_previewBack, m_previewContinue);
    setTabOrder(m_previewContinue, m_confirmDelete);

    updateControls();
    updateVisible();
}

void HighFilterPage::loadState()
{
    if (m_dataDir.isEmpty())
        return;
    QFile file(stateFilePath(m_dataDir));
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return;
    const QJsonObject obj = doc.object();
    m_loading = true;
    m_terms->setPlainText(obj.value(QStringLiteral("terms")).toString());
    m_directory->setText(obj.value(QStringLiteral("directory")).toString());
    m_loading = false;
}

void HighFilterPage::saveState() const
{
    if (m_dataDir.isEmpty())
        return;
    QJsonObject obj;
    obj.insert(QStringLiteral("terms"), m_terms->toPlainText());
    obj.insert(QStringLiteral("directory"), m_directory->text());
    QSaveFile file(stateFilePath(m_dataDir));
    if (!file.open(QIODevice::WriteOnly)) {
        m_services.logWarning(QStringLiteral("无法保存本工具状态：%1").arg(file.errorString()));
        return;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    if (!file.commit())
        m_services.logWarning(QStringLiteral("无法保存本工具状态：%1").arg(file.errorString()));
}

void HighFilterPage::refreshTermCount()
{
    const int count = distillTerms(m_terms->toPlainText()).size();
    m_termCount->setText(QStringLiteral("已识别 %1 个关键词").arg(count));
}

void HighFilterPage::startScan()
{
    if (m_busy || m_stage != Stage::Hidden)
        return;
    QString usable;
    const QString refusal = rootRefusal(m_directory->text(), &usable);
    if (!refusal.isEmpty()) {
        m_detail->setText(refusal);
        m_services.showStatus(StatusState::Error, refusal);
        m_services.logError(refusal);
        return;
    }
    saveState();
    auto job = std::make_shared<Job>();
    job->kind = Job::Kind::Walk;
    job->root = usable;
    job->terms = distillTerms(m_terms->toPlainText());
    startJob(job, QStringLiteral("正在筛选目录"), QStringLiteral("正在筛选目录"));
}

void HighFilterPage::cancelTask()
{
    if (!m_busy || !m_job)
        return;
    m_job->cancel.store(true);
    m_cancel->setEnabled(false);
    m_detail->setText(QStringLiteral("正在取消…"));
}

void HighFilterPage::browseDirectory()
{
    const QString current = m_directory->text().trimmed();
    const QString startHere = QFileInfo(current).isDir() ? current : QDir::homePath();
    const QString picked = QFileDialog::getExistingDirectory(
        m_dialogParent, QStringLiteral("选择要筛选的目录"), startHere);
    if (picked.isEmpty())
        return;
    m_directory->setText(QDir::cleanPath(picked));
    const QString refusal = rootRefusal(m_directory->text(), nullptr);
    if (!refusal.isEmpty()) {
        m_detail->setText(refusal);
        m_services.showStatus(StatusState::Error, refusal);
    }
}

void HighFilterPage::importFromDialog()
{
    const QString path = QFileDialog::getOpenFileName(
        m_dialogParent, QStringLiteral("导入名单"), QString(),
        QStringLiteral("文本文件 (*.txt *.list);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!loadTermsFromFile(path, &error)) {
        m_detail->setText(error);
        m_services.showStatus(StatusState::Error, error);
        m_services.logError(error);
    }
}

void HighFilterPage::onTermsEdited()
{
    refreshTermCount();
    if (m_loading || m_busy)
        return;
    saveState();
    clearResults(QStringLiteral("名单已改变，请重新筛选"));
}

void HighFilterPage::onDirectoryEdited()
{
    if (m_loading || m_busy)
        return;
    saveState();
    updateControls();
    if (!m_scannedRoot.isEmpty() || (m_files && m_files->rowCount() > 0))
        clearResults(QStringLiteral("目录已改变，请重新筛选"));
}

void HighFilterPage::onSearchChanged(const QString &text)
{
    if (!m_proxy)
        return;
    m_proxy->setNeedle(text);
    updateVisible();
}

void HighFilterPage::onModeChanged(int index)
{
    if (!m_proxy)
        return;
    m_proxy->setMode(index);
    updateVisible();
}

void HighFilterPage::recount()
{
    m_matched = 0;
    m_unmatched = 0;
    if (m_files) {
        for (int row = 0; row < m_files->rowCount(); ++row) {
            const ListedFile *file = m_files->fileAt(row);
            if (!file)
                continue;
            if (file->matched)
                ++m_matched;
            else
                ++m_unmatched;
        }
    }
    m_issueCount = m_issueModel ? m_issueModel->rowCount() : 0;
    m_matchedCount->setText(QString::number(m_matched));
    m_unmatchedCount->setText(QString::number(m_unmatched));
    m_totalCount->setText(QString::number(m_matched + m_unmatched));
    m_issueLabel->setText(QString::number(m_issueCount));
    updateVisible();
}

void HighFilterPage::updateVisible()
{
    const int shown = m_proxy ? m_proxy->rowCount() : 0;
    m_visibleCount->setText(QStringLiteral("可见 %1").arg(shown));
}

void HighFilterPage::updateControls()
{
    const bool frozen = m_busy || m_stage != Stage::Hidden;
    m_terms->setReadOnly(frozen);
    m_directory->setReadOnly(frozen);
    m_import->setEnabled(!frozen);
    m_browse->setEnabled(!frozen);
    m_start->setEnabled(!frozen && !m_directory->text().trimmed().isEmpty());
    m_cancel->setEnabled(m_busy);
    m_deleteMatched->setEnabled(!frozen && m_matched > 0);
    m_deleteUnmatched->setEnabled(!frozen && m_unmatched > 0);
    m_search->setEnabled(!m_busy);
    m_viewMode->setEnabled(!m_busy);
}

void HighFilterPage::beginPreview(bool matched)
{
    if (m_busy || m_stage != Stage::Hidden || !m_files)
        return;
    m_pending = m_files->pathsWhere(matched);
    if (m_pending.isEmpty())
        return;
    m_previewMatched = matched;
    setStage(Stage::Review);
}

void HighFilterPage::continuePreview()
{
    if (m_stage != Stage::Review)
        return;
    setStage(Stage::Confirm);
}

void HighFilterPage::backToReview()
{
    if (m_stage != Stage::Confirm)
        return;
    setStage(Stage::Review);
}

void HighFilterPage::cancelPreview()
{
    if (m_stage == Stage::Hidden)
        return;
    setStage(Stage::Hidden);
    m_detail->setText(QStringLiteral("已取消，目录没有改动"));
}

void HighFilterPage::confirmDelete()
{
    if (m_stage != Stage::Confirm || m_busy || m_pending.isEmpty() || m_scannedRoot.isEmpty())
        return;
    const QStringList paths = m_pending;
    const QString root = m_scannedRoot;
    setStage(Stage::Hidden);
    auto job = std::make_shared<Job>();
    job->kind = Job::Kind::Remove;
    job->root = root;
    job->paths = paths;
    startJob(job, QStringLiteral("正在删除文件"), QStringLiteral("正在删除文件"));
}

void HighFilterPage::startJob(const std::shared_ptr<Job> &job, const QString &lockReason,
                              const QString &statusText)
{
    if (m_busy || !job)
        return;
    m_busy = true;
    m_job = job;
    job->paceMs = m_paceMs;
    m_lock = m_services.lockNavigation(lockReason);
    m_services.showStatus(StatusState::Busy, statusText);
    m_services.logInfo(statusText);
    m_detail->setText(statusText);
    m_progress->setRange(0, 0);
    updateControls();

    HighFilterPage *page = this;
    m_thread = QThread::create([job, page]() {
        if (job->kind == Job::Kind::Walk) {
            job->walk = walkTree(job->root, job->terms, job->cancel, job->paceMs,
                                 [page](int files, int issues) {
                                     QMetaObject::invokeMethod(
                                         page,
                                         [page, files, issues]() { page->showPace(files, issues); },
                                         Qt::QueuedConnection);
                                 });
        } else {
            job->removal = removeListed(job->root, job->paths, job->cancel, job->paceMs,
                                        [page](int done, int total) {
                                            QMetaObject::invokeMethod(
                                                page,
                                                [page, done, total]() {
                                                    page->showRemovalPace(done, total);
                                                },
                                                Qt::QueuedConnection);
                                        });
        }
    });
    connect(m_thread, &QThread::finished, this, &HighFilterPage::onThreadFinished);
    m_thread->start();
}

void HighFilterPage::onThreadFinished()
{
    const std::shared_ptr<Job> job = std::move(m_job);
    if (m_thread) {
        m_thread->deleteLater();
        m_thread = nullptr;
    }
    m_busy = false;
    m_lock.release();
    if (m_progress) {
        m_progress->setRange(0, 1);
        m_progress->setValue(0);
    }
    if (m_aborting || !job) {
        updateControls();
        return;
    }
    if (job->kind == Job::Kind::Walk)
        applyWalk(job->walk);
    else
        applyRemoval(job->removal);
    updateControls();
}

void HighFilterPage::applyWalk(const TreeWalk &result)
{
    if (result.cancelled) {
        const QString text = QStringLiteral("已取消筛选");
        m_detail->setText(text);
        m_services.showStatus(StatusState::Info, text);
        m_services.logWarning(text);
        return;
    }
    if (!result.error.isEmpty()) {
        m_detail->setText(result.error);
        m_services.showStatus(StatusState::Error, result.error);
        m_services.logError(result.error);
        return;
    }

    m_scannedRoot = result.rootPath;
    m_files->setFiles(result.files);
    m_issueModel->setIssues(result.issues);
    recount();
    const QString text = QStringLiteral("筛选完成：匹配 %1，不匹配 %2，问题 %3")
                             .arg(m_matched)
                             .arg(m_unmatched)
                             .arg(m_issueCount);
    m_detail->setText(text);
    m_services.showStatus(StatusState::Success, text);
    m_services.logSuccess(text);
}

void HighFilterPage::applyRemoval(const RemovalBatch &result)
{
    if (!result.error.isEmpty()) {
        m_detail->setText(result.error);
        m_services.showStatus(StatusState::Error, result.error);
        m_services.logError(result.error);
        return;
    }

    QStringList gone;
    int failed = 0;
    for (const RemovalNote &note : result.items) {
        if (note.removed) {
            gone.append(note.absolutePath);
            continue;
        }
        ++failed;
        m_issueModel->appendIssue(TreeIssue{note.absolutePath, note.error});
        m_services.logError(QStringLiteral("%1：%2").arg(note.absolutePath, note.error));
    }
    if (!gone.isEmpty())
        m_files->removePaths(gone);
    recount();

    if (result.cancelled) {
        const QString text = QStringLiteral("已取消删除");
        m_detail->setText(text);
        m_services.showStatus(StatusState::Info, text);
        m_services.logWarning(text);
        return;
    }
    if (failed > 0) {
        const QString text =
            QStringLiteral("删除结束：成功 %1 个，失败 %2 个").arg(gone.size()).arg(failed);
        m_detail->setText(text);
        m_services.showStatus(StatusState::Error, text);
        return;
    }
    const QString text = QStringLiteral("已删除 %1 个文件").arg(gone.size());
    m_detail->setText(text);
    m_services.showStatus(StatusState::Success, text);
    m_services.logSuccess(text);
}

void HighFilterPage::clearResults(const QString &detail)
{
    if (m_stage != Stage::Hidden)
        setStage(Stage::Hidden);
    const bool had = (m_files && m_files->rowCount() > 0) || (m_issueModel && m_issueModel->rowCount() > 0)
        || !m_scannedRoot.isEmpty();
    if (m_files)
        m_files->setFiles({});
    if (m_issueModel)
        m_issueModel->setIssues({});
    m_scannedRoot.clear();
    recount();
    if (had && !detail.isEmpty())
        m_detail->setText(detail);
}

void HighFilterPage::setStage(Stage stage)
{
    m_stage = stage;
    if (stage == Stage::Hidden) {
        m_stack->setCurrentIndex(0);
        m_previewList->clear();
        m_pending.clear();
        m_previewBack->setEnabled(false);
        m_previewContinue->setEnabled(false);
        m_confirmDelete->setEnabled(false);
        updateControls();
        return;
    }

    m_stack->setCurrentWidget(m_preview);
    const QString kind = m_previewMatched ? QStringLiteral("匹配") : QStringLiteral("不匹配");
    if (m_previewList->count() != m_pending.size()) {
        m_previewList->clear();
        for (const QString &path : m_pending)
            m_previewList->addItem(path);
    }
    if (stage == Stage::Review) {
        m_previewTitle->setText(
            QStringLiteral("预览将删除的%1文件（共 %2 个）").arg(kind).arg(m_pending.size()));
        m_previewHint->setText(QStringLiteral("请核对完整列表。这一步不会修改目录，也不会删除目录本身。"));
        m_previewBack->setEnabled(false);
        m_previewContinue->setEnabled(true);
        m_confirmDelete->setEnabled(false);
    } else {
        m_previewTitle->setText(
            QStringLiteral("第二次确认：删除这 %1 个%2文件").arg(m_pending.size()).arg(kind));
        m_previewHint->setText(QStringLiteral("确认后才会删除下列文件。未确认不会改动任何文件。"));
        m_previewBack->setEnabled(true);
        m_previewContinue->setEnabled(false);
        m_confirmDelete->setEnabled(true);
    }
    m_previewCancel->setEnabled(true);
    updateControls();
}

void HighFilterPage::showPace(int files, int issues)
{
    if (!m_busy)
        return;
    m_detail->setText(QStringLiteral("已看到 %1 个文件，%2 个问题").arg(files).arg(issues));
}

void HighFilterPage::showRemovalPace(int done, int total)
{
    if (!m_busy)
        return;
    m_detail->setText(QStringLiteral("正在删除 %1 / %2").arg(done).arg(total));
}

} // namespace tw
