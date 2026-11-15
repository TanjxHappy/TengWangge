#include "tools/filter_unique_high/high_filter_page.h"

#include "shell/elided_label.h"
#include "tools/filter_unique_high/bounded_tree.h"
#include "tools/filter_unique_high/term_list.h"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSet>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTableView>
#include <QTextCodec>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <atomic>
#include <functional>

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

    void setScope(const QString &scope)
    {
        m_scope = scope;
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
        if (!m_scope.isEmpty()) {
            const QString prefix = m_scope + QLatin1Char('/');
            if (!file->relativePath.startsWith(prefix))
                return false;
        }
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
    QString m_scope;
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
    root->setContentsMargins(6, 4, 6, 8);
    root->setSpacing(4);

    auto *bar = new QFrame;
    bar->setObjectName(QStringLiteral("highFilter.toolbar"));
    bar->setProperty("role", QStringLiteral("card"));
    auto *barLayout = new QVBoxLayout(bar);
    barLayout->setContentsMargins(6, 4, 6, 4);
    barLayout->setSpacing(4);

    m_termsDialog = new QDialog(this);
    m_termsDialog->setObjectName(QStringLiteral("highFilter.termsDialog"));
    m_termsDialog->setWindowTitle(QStringLiteral("关键词名单"));
    m_termsDialog->setModal(false);
    m_termsDialog->resize(520, 360);
    auto *termsLayout = new QVBoxLayout(m_termsDialog);
    termsLayout->setSpacing(6);
    auto *termsHint = makeLabel(
        QStringLiteral("每行一个关键词。空行忽略，重复项按大小写不敏感合并，保留第一次的写法。"),
        "hint");
    termsHint->setWordWrap(true);
    m_terms = new QPlainTextEdit;
    m_terms->setObjectName(QStringLiteral("highFilter.terms"));
    m_terms->setTabChangesFocus(true);
    m_terms->setPlaceholderText(QStringLiteral("每行一个关键词"));
    m_terms->setAccessibleName(QStringLiteral("关键词名单"));
    m_import = new QPushButton(QStringLiteral("导入 UTF-8 文本…"));
    m_import->setObjectName(QStringLiteral("highFilter.import"));
    m_import->setAccessibleName(QStringLiteral("导入名单"));
    auto *closeTerms = new QPushButton(QStringLiteral("完成"));
    closeTerms->setProperty("role", QStringLiteral("primary"));
    auto *termsButtons = new QHBoxLayout;
    termsButtons->setSpacing(8);
    termsButtons->addWidget(m_import);
    termsButtons->addStretch(1);
    termsButtons->addWidget(closeTerms);
    termsLayout->addWidget(termsHint);
    termsLayout->addWidget(m_terms, 1);
    termsLayout->addLayout(termsButtons);

    m_termsButton = new QPushButton(QStringLiteral("名单"));
    m_termsButton->setObjectName(QStringLiteral("highFilter.termsButton"));
    m_termsButton->setAccessibleName(QStringLiteral("编辑关键词名单"));
    m_termsButton->setToolTip(QStringLiteral("粘贴或导入名单。空行忽略，大小写不敏感去重。"));
    m_termCount = makeLabel(QStringLiteral("已识别 0 个关键词"), "hint");
    m_termCount->setObjectName(QStringLiteral("highFilter.termCount"));

    m_directory = new QLineEdit;
    m_directory->setObjectName(QStringLiteral("highFilter.directory"));
    m_directory->setPlaceholderText(QStringLiteral("绝对路径目录，不能是 /"));
    m_directory->setAccessibleName(QStringLiteral("要筛选的目录"));
    m_browse = new QPushButton(QStringLiteral("浏览…"));
    m_browse->setObjectName(QStringLiteral("highFilter.browse"));
    m_browse->setAccessibleName(QStringLiteral("浏览目录"));
    m_start = new QPushButton(QStringLiteral("开始筛选"));
    m_start->setObjectName(QStringLiteral("highFilter.start"));
    m_start->setProperty("role", QStringLiteral("primary"));
    m_start->setAccessibleName(QStringLiteral("开始筛选"));
    m_start->setToolTip(QStringLiteral(
        "不区分大小写的子串匹配相对路径和文件名，不读内容。符号链接不越界、不循环。"));
    m_cancel = new QPushButton(QStringLiteral("取消"));
    m_cancel->setObjectName(QStringLiteral("highFilter.cancel"));
    m_cancel->setAccessibleName(QStringLiteral("取消当前任务"));
    m_progress = new QProgressBar;
    m_progress->setObjectName(QStringLiteral("highFilter.progress"));
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setFixedWidth(88);
    m_progress->setAccessibleName(QStringLiteral("筛选进度"));

    auto *commandRow = new QHBoxLayout;
    commandRow->setSpacing(6);
    commandRow->addWidget(m_termsButton);
    commandRow->addWidget(m_termCount);
    commandRow->addWidget(makeLabel(QStringLiteral("目录"), "fieldLabel"));
    commandRow->addWidget(m_directory, 1);
    commandRow->addWidget(m_browse);
    commandRow->addWidget(m_start);
    commandRow->addWidget(m_cancel);
    commandRow->addWidget(m_progress);
    barLayout->addLayout(commandRow);

    m_matchedCount = makeLabel(QStringLiteral("0"), "value");
    m_matchedCount->setObjectName(QStringLiteral("highFilter.matchedCount"));
    m_unmatchedCount = makeLabel(QStringLiteral("0"), "value");
    m_unmatchedCount->setObjectName(QStringLiteral("highFilter.unmatchedCount"));
    m_totalCount = makeLabel(QStringLiteral("0"), "value");
    m_totalCount->setObjectName(QStringLiteral("highFilter.totalCount"));
    m_issueLabel = makeLabel(QStringLiteral("0"), "value");
    m_issueLabel->setObjectName(QStringLiteral("highFilter.issueCount"));
    m_deleteMatched = new QPushButton(QStringLiteral("删除匹配"));
    m_deleteMatched->setObjectName(QStringLiteral("highFilter.deleteMatched"));
    m_deleteMatched->setProperty("role", QStringLiteral("danger"));
    m_deleteMatched->setAccessibleName(QStringLiteral("删除匹配文件"));
    m_deleteMatched->setToolTip(QStringLiteral("先预览完整列表，再做第二次确认。不会删除目录。"));
    m_deleteUnmatched = new QPushButton(QStringLiteral("删除不匹配"));
    m_deleteUnmatched->setObjectName(QStringLiteral("highFilter.deleteUnmatched"));
    m_deleteUnmatched->setProperty("role", QStringLiteral("danger"));
    m_deleteUnmatched->setAccessibleName(QStringLiteral("删除不匹配文件"));
    m_deleteUnmatched->setToolTip(QStringLiteral("先预览完整列表，再做第二次确认。不会删除目录。"));

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
    m_rule = new ElidedLabel;
    m_rule->setObjectName(QStringLiteral("highFilter.rule"));
    m_rule->setProperty("role", QStringLiteral("hint"));
    m_rule->setAccessibleName(QStringLiteral("筛选规则"));
    m_rule->setFullText(QStringLiteral(
        "不区分大小写的子串匹配相对路径和文件名，不读内容。符号链接不越界、不循环。"
        "删除前预览完整列表并第二次确认，未确认不改目录。"));
    m_detail = makeLabel(QStringLiteral("尚未筛选"), "hint");
    m_detail->setObjectName(QStringLiteral("highFilter.detail"));
    m_detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_detail->setMinimumWidth(0);

    auto *filterRow = new QHBoxLayout;
    filterRow->setSpacing(6);
    filterRow->addWidget(makeLabel(QStringLiteral("搜索"), "fieldLabel"));
    filterRow->addWidget(m_search, 1);
    filterRow->addWidget(makeLabel(QStringLiteral("显示"), "fieldLabel"));
    filterRow->addWidget(m_viewMode);
    filterRow->addWidget(m_visibleCount);
    filterRow->addWidget(makeLabel(QStringLiteral("匹配"), "fieldLabel"));
    filterRow->addWidget(m_matchedCount);
    filterRow->addWidget(makeLabel(QStringLiteral("不匹配"), "fieldLabel"));
    filterRow->addWidget(m_unmatchedCount);
    filterRow->addWidget(makeLabel(QStringLiteral("总数"), "fieldLabel"));
    filterRow->addWidget(m_totalCount);
    filterRow->addWidget(makeLabel(QStringLiteral("问题"), "fieldLabel"));
    filterRow->addWidget(m_issueLabel);
    filterRow->addWidget(m_deleteMatched);
    filterRow->addWidget(m_deleteUnmatched);
    barLayout->addLayout(filterRow);

    auto *hintRow = new QHBoxLayout;
    hintRow->setSpacing(8);
    hintRow->addWidget(m_rule, 1);
    hintRow->addWidget(m_detail, 1);
    barLayout->addLayout(hintRow);
    root->addWidget(bar);

    m_treeModel = new QStandardItemModel(this);
    m_treeModel->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("结果")});
    m_tree = new QTreeView;
    m_tree->setObjectName(QStringLiteral("highFilter.tree"));
    m_tree->setModel(m_treeModel);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setTextElideMode(Qt::ElideMiddle);
    m_tree->setMinimumWidth(160);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setAccessibleName(QStringLiteral("目录树"));
    m_scopeLabel = new ElidedLabel;
    m_scopeLabel->setObjectName(QStringLiteral("highFilter.scope"));
    m_scopeLabel->setProperty("role", QStringLiteral("hint"));
    m_scopeLabel->setFullText(QStringLiteral("范围：全部"));
    m_scopeReset = new QPushButton(QStringLiteral("返回"));
    m_scopeReset->setObjectName(QStringLiteral("highFilter.scopeReset"));
    m_scopeReset->setVisible(false);
    m_scopeReset->setToolTip(QStringLiteral("取消目录范围，重新显示全部扫描结果"));
    auto *treePane = new QWidget;
    treePane->setObjectName(QStringLiteral("highFilter.treePane"));
    treePane->setMinimumWidth(160);
    auto *treeLayout = new QVBoxLayout(treePane);
    treeLayout->setContentsMargins(0, 0, 0, 0);
    treeLayout->setSpacing(2);
    auto *treeHead = new QHBoxLayout;
    treeHead->setSpacing(4);
    treeHead->addWidget(makeLabel(QStringLiteral("目录树"), "fieldLabel"));
    treeHead->addWidget(m_scopeLabel, 1);
    treeHead->addWidget(m_scopeReset);
    treeLayout->addLayout(treeHead);
    treeLayout->addWidget(m_tree, 1);

    m_files = new HitModel(this);
    m_proxy = new HitProxy(m_files, this);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setSortRole(Qt::DisplayRole);
    m_results = new QTableView;
    m_results->setObjectName(QStringLiteral("highFilter.results"));
    m_results->setModel(m_proxy);
    m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results->setShowGrid(true);
    m_results->setAlternatingRowColors(true);
    m_results->setWordWrap(false);
    m_results->setTextElideMode(Qt::ElideMiddle);
    m_results->verticalHeader()->setVisible(false);
    m_results->horizontalHeader()->setStretchLastSection(false);
    m_results->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_results->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_results->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_results->horizontalHeader()->setSortIndicatorShown(true);
    m_results->setSortingEnabled(true);
    m_results->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_results->setMinimumWidth(480);
    m_results->setMinimumHeight(160);
    m_results->setAccessibleName(QStringLiteral("筛选结果"));
    m_results->setToolTip(QStringLiteral("单击与目录树、详情联动。双击打开详情。点击表头排序。"));
    auto *resultPane = new QWidget;
    resultPane->setObjectName(QStringLiteral("highFilter.resultPane"));
    resultPane->setMinimumWidth(480);
    auto *resultLayout = new QVBoxLayout(resultPane);
    resultLayout->setContentsMargins(0, 0, 0, 0);
    resultLayout->setSpacing(2);
    resultLayout->addWidget(makeLabel(QStringLiteral("匹配结果"), "fieldLabel"));
    resultLayout->addWidget(m_results, 1);

    m_focusPath = new ElidedLabel;
    m_focusPath->setObjectName(QStringLiteral("highFilter.focusPath"));
    m_focusPath->setProperty("role", QStringLiteral("value"));
    m_focusPath->setFullText(QStringLiteral("未选择文件"));
    m_focusState = makeLabel(QStringLiteral("单击一行查看"), "hint");
    m_focusState->setObjectName(QStringLiteral("highFilter.focusState"));
    m_focusHits = new ElidedLabel;
    m_focusHits->setObjectName(QStringLiteral("highFilter.focusHits"));
    m_focusHits->setProperty("role", QStringLiteral("hint"));
    m_focusHits->setFullText(QStringLiteral("命中的名单项会显示在这里"));
    m_openFile = new QPushButton(QStringLiteral("打开"));
    m_openFile->setObjectName(QStringLiteral("highFilter.openFile"));
    m_openFile->setEnabled(false);
    m_openFile->setToolTip(QStringLiteral("用系统默认程序打开这个文件"));
    m_showDetail = new QPushButton(QStringLiteral("详情"));
    m_showDetail->setObjectName(QStringLiteral("highFilter.showDetail"));
    m_showDetail->setEnabled(false);
    m_locate = new QPushButton(QStringLiteral("定位"));
    m_locate->setObjectName(QStringLiteral("highFilter.locate"));
    m_locate->setEnabled(false);
    m_locate->setToolTip(QStringLiteral("在目录树和结果表中定位当前文件"));
    auto *detailPane = new QWidget;
    detailPane->setObjectName(QStringLiteral("highFilter.detailPane"));
    detailPane->setMinimumWidth(150);
    detailPane->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto *detailLayout = new QVBoxLayout(detailPane);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(4);
    detailLayout->addWidget(makeLabel(QStringLiteral("选中项"), "fieldLabel"));
    detailLayout->addWidget(m_focusPath);
    detailLayout->addWidget(m_focusState);
    detailLayout->addWidget(m_focusHits);
    m_openFile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_showDetail->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_locate->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    detailLayout->addWidget(m_openFile);
    detailLayout->addWidget(m_showDetail);
    detailLayout->addWidget(m_locate);
    detailLayout->addStretch(1);

    m_issueModel = new IssueModel(this);
    auto *issueProxy = new QSortFilterProxyModel(this);
    issueProxy->setSourceModel(m_issueModel);
    issueProxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_issues = new QTableView;
    m_issues->setObjectName(QStringLiteral("highFilter.issues"));
    m_issues->setModel(issueProxy);
    m_issues->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_issues->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_issues->setSelectionMode(QAbstractItemView::SingleSelection);
    m_issues->setShowGrid(true);
    m_issues->setAlternatingRowColors(true);
    m_issues->setWordWrap(false);
    m_issues->setTextElideMode(Qt::ElideMiddle);
    m_issues->verticalHeader()->setVisible(false);
    m_issues->horizontalHeader()->setStretchLastSection(true);
    m_issues->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_issues->horizontalHeader()->setSortIndicatorShown(true);
    m_issues->setSortingEnabled(true);
    m_issues->setMinimumHeight(56);
    m_issues->setAccessibleName(QStringLiteral("问题列表"));
    m_issues->setToolTip(QStringLiteral("单击问题可跳到对应文件；没有对应文件时在右侧显示原因。"));
    auto *issuePane = new QWidget;
    issuePane->setObjectName(QStringLiteral("highFilter.issuePane"));
    issuePane->setMinimumHeight(80);
    auto *issueLayout = new QVBoxLayout(issuePane);
    issueLayout->setContentsMargins(0, 0, 0, 0);
    issueLayout->setSpacing(2);
    issueLayout->addWidget(makeLabel(QStringLiteral("问题"), "fieldLabel"));
    issueLayout->addWidget(m_issues, 1);

    m_workSplit = new QSplitter(Qt::Horizontal);
    m_workSplit->setObjectName(QStringLiteral("highFilter.workSplit"));
    m_workSplit->setChildrenCollapsible(false);
    m_workSplit->addWidget(treePane);
    m_workSplit->addWidget(resultPane);
    m_workSplit->addWidget(detailPane);
    m_workSplit->setStretchFactor(0, 4);
    m_workSplit->setStretchFactor(1, 5);
    m_workSplit->setStretchFactor(2, 2);

    m_mainSplit = new QSplitter(Qt::Vertical);
    m_mainSplit->setObjectName(QStringLiteral("highFilter.mainSplit"));
    m_mainSplit->setChildrenCollapsible(false);
    m_mainSplit->addWidget(m_workSplit);
    m_mainSplit->addWidget(issuePane);
    m_mainSplit->setStretchFactor(0, 5);
    m_mainSplit->setStretchFactor(1, 1);
    root->addWidget(m_mainSplit, 1);

    m_preview = new QDialog(this);
    m_preview->setObjectName(QStringLiteral("highFilter.preview"));
    m_preview->setWindowTitle(QStringLiteral("删除预览"));
    m_preview->setModal(false);
    m_preview->resize(680, 460);
    auto *previewLayout = new QVBoxLayout(m_preview);
    previewLayout->setSpacing(8);
    m_previewTitle = makeLabel(QStringLiteral("预览"), "sectionTitle");
    m_previewTitle->setObjectName(QStringLiteral("highFilter.previewTitle"));
    m_previewHint = makeLabel(QString(), "hint");
    m_previewHint->setObjectName(QStringLiteral("highFilter.previewHint"));
    m_previewHint->setWordWrap(true);
    m_previewList = new QListWidget;
    m_previewList->setObjectName(QStringLiteral("highFilter.previewList"));
    m_previewList->setAccessibleName(QStringLiteral("待删除文件完整列表"));
    m_previewList->setAlternatingRowColors(true);
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
    previewButtons->addStretch(1);
    previewButtons->addWidget(m_previewContinue);
    previewButtons->addWidget(m_confirmDelete);
    previewLayout->addWidget(m_previewTitle);
    previewLayout->addWidget(m_previewHint);
    previewLayout->addWidget(m_previewList, 1);
    previewLayout->addLayout(previewButtons);

    m_fileDialog = new QDialog(this);
    m_fileDialog->setObjectName(QStringLiteral("highFilter.fileDialog"));
    m_fileDialog->setWindowTitle(QStringLiteral("文件详情"));
    m_fileDialog->setModal(false);
    m_fileDialog->resize(560, 240);
    m_dialogPath = makeLabel(QString(), "value");
    m_dialogPath->setObjectName(QStringLiteral("highFilter.dialogPath"));
    m_dialogPath->setWordWrap(true);
    m_dialogPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_dialogState = makeLabel(QString(), "hint");
    m_dialogState->setObjectName(QStringLiteral("highFilter.dialogState"));
    m_dialogHits = makeLabel(QString(), "hint");
    m_dialogHits->setObjectName(QStringLiteral("highFilter.dialogHits"));
    m_dialogHits->setWordWrap(true);
    auto *dialogOpen = new QPushButton(QStringLiteral("打开文件"));
    dialogOpen->setProperty("role", QStringLiteral("primary"));
    auto *dialogLocate = new QPushButton(QStringLiteral("在树中定位"));
    auto *dialogClose = new QPushButton(QStringLiteral("关闭"));
    auto *dialogButtons = new QHBoxLayout;
    dialogButtons->setSpacing(8);
    dialogButtons->addWidget(dialogOpen);
    dialogButtons->addWidget(dialogLocate);
    dialogButtons->addStretch(1);
    dialogButtons->addWidget(dialogClose);
    auto *fileLayout = new QVBoxLayout(m_fileDialog);
    fileLayout->setSpacing(8);
    fileLayout->addWidget(makeLabel(QStringLiteral("文件详情"), "sectionTitle"));
    fileLayout->addWidget(m_dialogPath);
    fileLayout->addWidget(m_dialogState);
    fileLayout->addWidget(m_dialogHits);
    fileLayout->addStretch(1);
    fileLayout->addLayout(dialogButtons);

    loadState();
    refreshTermCount();

    connect(m_terms, &QPlainTextEdit::textChanged, this, &HighFilterPage::onTermsEdited);
    connect(m_directory, &QLineEdit::textChanged, this, &HighFilterPage::onDirectoryEdited);
    connect(m_termsButton, &QPushButton::clicked, this, &HighFilterPage::editTerms);
    connect(closeTerms, &QPushButton::clicked, m_termsDialog, &QDialog::hide);
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
    connect(m_scopeReset, &QPushButton::clicked, this, [this]() { applyScope(QString()); });
    connect(m_openFile, &QPushButton::clicked, this, &HighFilterPage::openFocusedFile);
    connect(m_showDetail, &QPushButton::clicked, this, &HighFilterPage::openFocusDialog);
    connect(m_locate, &QPushButton::clicked, this, &HighFilterPage::locateFocusedFile);
    connect(dialogOpen, &QPushButton::clicked, this, &HighFilterPage::openFocusedFile);
    connect(dialogLocate, &QPushButton::clicked, this, &HighFilterPage::locateFocusedFile);
    connect(dialogClose, &QPushButton::clicked, m_fileDialog, &QDialog::hide);
    connect(m_proxy, &QAbstractItemModel::layoutChanged, this, &HighFilterPage::rebuildTree);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, &HighFilterPage::rebuildTree);
    connect(m_results->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &, const QModelIndex &) { onTableSelectionChanged(); });
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &, const QModelIndex &) { onTreeSelectionChanged(); });
    connect(m_tree, &QTreeView::doubleClicked, this, &HighFilterPage::onTreeDoubleClicked);
    connect(m_results, &QTableView::doubleClicked, this,
            [this](const QModelIndex &) { openFocusDialog(); });
    connect(m_issues, &QTableView::clicked, this, &HighFilterPage::onIssueActivated);

    setTabOrder(m_termsButton, m_directory);
    setTabOrder(m_directory, m_browse);
    setTabOrder(m_browse, m_start);
    setTabOrder(m_start, m_cancel);
    setTabOrder(m_cancel, m_search);
    setTabOrder(m_search, m_viewMode);
    setTabOrder(m_viewMode, m_deleteMatched);
    setTabOrder(m_deleteMatched, m_deleteUnmatched);
    setTabOrder(m_deleteUnmatched, m_tree);
    setTabOrder(m_tree, m_results);
    setTabOrder(m_results, m_issues);

    updateControls();
    updateVisible();
    rebuildTree();
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
    applyScope(QString());
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
    applyScope(QString());
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
        m_preview->hide();
        m_previewList->clear();
        m_pending.clear();
        m_previewBack->setEnabled(false);
        m_previewContinue->setEnabled(false);
        m_previewContinue->setDefault(false);
        m_confirmDelete->setEnabled(false);
        m_confirmDelete->setDefault(false);
        updateControls();
        return;
    }
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
        m_preview->setWindowTitle(QStringLiteral("删除预览"));
        m_previewBack->setEnabled(false);
        m_previewContinue->setEnabled(true);
        m_previewContinue->setDefault(true);
        m_confirmDelete->setEnabled(false);
        m_confirmDelete->setDefault(false);
    } else {
        m_previewTitle->setText(
            QStringLiteral("第二次确认：删除这 %1 个%2文件").arg(m_pending.size()).arg(kind));
        m_previewHint->setText(QStringLiteral("确认后才会删除下列文件。未确认不会改动任何文件。"));
        m_preview->setWindowTitle(QStringLiteral("第二次确认"));
        m_previewBack->setEnabled(true);
        m_previewContinue->setEnabled(false);
        m_previewContinue->setDefault(false);
        m_confirmDelete->setEnabled(true);
        m_confirmDelete->setDefault(true);
    }
    m_previewCancel->setEnabled(true);
    m_preview->show();
    m_preview->raise();
    QCoreApplication::processEvents();
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

void HighFilterPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_splitReady)
        return;
    QTimer::singleShot(0, this, [this]() { applySplitSizes(); });
}

void HighFilterPage::applySplitSizes()
{
    if (!m_workSplit || !m_mainSplit)
        return;
    const int width = m_workSplit->width();
    const int height = m_mainSplit->height();
    if (width < 700 || height < 240)
        return;
    m_splitReady = true;
    const int detail = qMax(176, width / 8);
    int tree = qMax(280, width * 32 / 100);
    int table = width - tree - detail;
    if (table < 500) {
        tree = qMax(200, width - detail - 500);
        table = width - tree - detail;
    }
    m_workSplit->setSizes({tree, table, detail});
    const int issues = qBound(88, height / 5, 140);
    m_mainSplit->setSizes({height - issues, issues});
}

void HighFilterPage::editTerms()
{
    if (!m_termsDialog)
        return;
    m_termsDialog->show();
    m_termsDialog->raise();
    m_terms->setFocus();
}

void HighFilterPage::applyScope(const QString &scope)
{
    m_scope = scope;
    if (m_proxy)
        m_proxy->setScope(scope);
    if (m_scopeLabel)
        m_scopeLabel->setFullText(scope.isEmpty() ? QStringLiteral("范围：全部") : scope);
    if (m_scopeReset)
        m_scopeReset->setVisible(!scope.isEmpty());
}

const ListedFile *HighFilterPage::fileByRelative(const QString &relative) const
{
    if (!m_files)
        return nullptr;
    for (int row = 0; row < m_files->rowCount(); ++row) {
        const ListedFile *file = m_files->fileAt(row);
        if (file && file->relativePath == relative)
            return file;
    }
    return nullptr;
}

const ListedFile *HighFilterPage::fileByAbsolute(const QString &absolute) const
{
    if (!m_files)
        return nullptr;
    for (int row = 0; row < m_files->rowCount(); ++row) {
        const ListedFile *file = m_files->fileAt(row);
        if (file && file->absolutePath == absolute)
            return file;
    }
    return nullptr;
}

void HighFilterPage::rebuildTree()
{
    if (!m_treeModel || !m_proxy || !m_files)
        return;
    const QString keep = m_focusRelative;
    m_fileNodes.clear();
    m_treeModel->clear();
    m_treeModel->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("结果")});

    QHash<QString, QStandardItem *> dirs;
    std::function<QStandardItem *(const QString &)> ensureDir;
    ensureDir = [&](const QString &path) -> QStandardItem * {
        if (path.isEmpty())
            return nullptr;
        if (QStandardItem *found = dirs.value(path))
            return found;
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        const QString parentPath = slash < 0 ? QString() : path.left(slash);
        const QString name = slash < 0 ? path : path.mid(slash + 1);
        auto *nameItem = new QStandardItem(name);
        nameItem->setEditable(false);
        nameItem->setData(path, Qt::UserRole);
        nameItem->setData(QStringLiteral("dir"), Qt::UserRole + 1);
        nameItem->setToolTip(path);
        auto *status = new QStandardItem;
        status->setEditable(false);
        QStandardItem *parent = ensureDir(parentPath);
        if (parent)
            parent->appendRow({nameItem, status});
        else
            m_treeModel->appendRow({nameItem, status});
        dirs.insert(path, nameItem);
        return nameItem;
    };

    for (int row = 0; row < m_proxy->rowCount(); ++row) {
        const QModelIndex source = m_proxy->mapToSource(m_proxy->index(row, 0));
        const ListedFile *file = m_files->fileAt(source.row());
        if (!file)
            continue;
        const int slash = file->relativePath.lastIndexOf(QLatin1Char('/'));
        const QString dir = slash < 0 ? QString() : file->relativePath.left(slash);
        QStandardItem *parent = ensureDir(dir);
        auto *nameItem = new QStandardItem(file->fileName);
        nameItem->setEditable(false);
        nameItem->setData(file->relativePath, Qt::UserRole);
        nameItem->setData(QStringLiteral("file"), Qt::UserRole + 1);
        nameItem->setToolTip(file->absolutePath);
        auto *status = new QStandardItem(file->matched ? QStringLiteral("匹配") : QStringLiteral("不匹配"));
        status->setEditable(false);
        if (parent)
            parent->appendRow({nameItem, status});
        else
            m_treeModel->appendRow({nameItem, status});
        m_fileNodes.insert(file->relativePath, nameItem);
    }

    for (QStandardItem *dirItem : dirs) {
        int files = 0;
        std::function<void(QStandardItem *)> count = [&](QStandardItem *item) {
            for (int i = 0; i < item->rowCount(); ++i) {
                QStandardItem *child = item->child(i, 0);
                if (!child)
                    continue;
                if (child->data(Qt::UserRole + 1).toString() == QLatin1String("file"))
                    ++files;
                else
                    count(child);
            }
        };
        count(dirItem);
        QStandardItem *statusItem = nullptr;
        if (QStandardItem *parent = dirItem->parent())
            statusItem = parent->child(dirItem->row(), 1);
        else
            statusItem = m_treeModel->item(dirItem->row(), 1);
        if (statusItem)
            statusItem->setText(QString::number(files));
    }

    if (m_tree) {
        m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_tree->expandAll();
    }
    if (!keep.isEmpty() && m_fileNodes.contains(keep))
        selectRelative(keep, true);
}

void HighFilterPage::selectRelative(const QString &relative, bool revealTree)
{
    if (relative.isEmpty() || m_linking)
        return;
    m_linking = true;
    if (revealTree && m_tree) {
        if (QStandardItem *item = m_fileNodes.value(relative)) {
            const QModelIndex index = item->index();
            m_tree->setCurrentIndex(index);
            m_tree->scrollTo(index);
        }
    }
    if (m_results && m_proxy) {
        for (int row = 0; row < m_proxy->rowCount(); ++row) {
            if (m_proxy->index(row, 0).data().toString() == relative) {
                m_results->selectRow(row);
                m_results->scrollTo(m_proxy->index(row, 0));
                break;
            }
        }
    }
    m_linking = false;
    showFocus(fileByRelative(relative));
}

void HighFilterPage::onTableSelectionChanged()
{
    if (m_linking || !m_results || !m_proxy || !m_files)
        return;
    const QModelIndex current = m_results->currentIndex();
    if (!current.isValid())
        return;
    const QModelIndex source = m_proxy->mapToSource(m_proxy->index(current.row(), 0));
    const ListedFile *file = m_files->fileAt(source.row());
    if (!file)
        return;
    m_linking = true;
    if (QStandardItem *item = m_fileNodes.value(file->relativePath)) {
        m_tree->setCurrentIndex(item->index());
        m_tree->scrollTo(item->index());
    }
    m_linking = false;
    showFocus(file);
}

void HighFilterPage::onTreeSelectionChanged()
{
    if (m_linking || !m_tree || !m_treeModel)
        return;
    const QModelIndex current = m_tree->currentIndex();
    if (!current.isValid())
        return;
    QStandardItem *item = m_treeModel->itemFromIndex(current.sibling(current.row(), 0));
    if (!item)
        return;
    if (item->data(Qt::UserRole + 1).toString() != QLatin1String("file")) {
        std::function<QStandardItem *(QStandardItem *)> firstFile = [&](QStandardItem *node) -> QStandardItem * {
            for (int i = 0; i < node->rowCount(); ++i) {
                QStandardItem *child = node->child(i, 0);
                if (!child)
                    continue;
                if (child->data(Qt::UserRole + 1).toString() == QLatin1String("file"))
                    return child;
                if (QStandardItem *nested = firstFile(child))
                    return nested;
            }
            return nullptr;
        };
        item = firstFile(item);
        if (!item)
            return;
    }
    selectRelative(item->data(Qt::UserRole).toString(), false);
}

void HighFilterPage::onTreeDoubleClicked(const QModelIndex &index)
{
    if (!m_treeModel || !index.isValid())
        return;
    QStandardItem *item = m_treeModel->itemFromIndex(index.sibling(index.row(), 0));
    if (!item)
        return;
    if (item->data(Qt::UserRole + 1).toString() == QLatin1String("dir")) {
        applyScope(item->data(Qt::UserRole).toString());
        return;
    }
    showFocus(fileByRelative(item->data(Qt::UserRole).toString()));
    openFocusDialog();
}

void HighFilterPage::onIssueActivated(const QModelIndex &index)
{
    if (!index.isValid() || !m_issues)
        return;
    const QString path = m_issues->model()->index(index.row(), 0).data().toString();
    const QString reason = m_issues->model()->index(index.row(), 1).data().toString();
    if (const ListedFile *file = fileByAbsolute(path)) {
        selectRelative(file->relativePath, true);
        return;
    }
    m_focusRelative.clear();
    m_focusPath->setFullText(path);
    m_focusState->setText(QStringLiteral("问题"));
    m_focusHits->setFullText(reason);
    m_openFile->setEnabled(false);
    m_showDetail->setEnabled(false);
    m_locate->setEnabled(false);
}

void HighFilterPage::showFocus(const ListedFile *file)
{
    if (!file) {
        m_focusRelative.clear();
        m_focusPath->setFullText(QStringLiteral("未选择文件"));
        m_focusState->setText(QStringLiteral("单击一行查看"));
        m_focusHits->setFullText(QStringLiteral("命中的名单项会显示在这里"));
        m_openFile->setEnabled(false);
        m_showDetail->setEnabled(false);
        m_locate->setEnabled(false);
        return;
    }
    m_focusRelative = file->relativePath;
    m_focusPath->setFullText(file->relativePath);
    m_focusState->setText(file->matched ? QStringLiteral("匹配") : QStringLiteral("不匹配"));
    const QString hits = file->hits.isEmpty() ? QStringLiteral("无命中名单项")
                                               : file->hits.join(QStringLiteral("、"));
    m_focusHits->setFullText(hits);
    m_openFile->setEnabled(true);
    m_showDetail->setEnabled(true);
    m_locate->setEnabled(true);
    if (m_fileDialog && m_fileDialog->isVisible())
        openFocusDialog();
}

void HighFilterPage::openFocusDialog()
{
    const ListedFile *file = fileByRelative(m_focusRelative);
    if (!file || !m_fileDialog)
        return;
    m_dialogPath->setText(file->absolutePath);
    m_dialogState->setText(file->matched ? QStringLiteral("结果：匹配") : QStringLiteral("结果：不匹配"));
    const QString hits = file->hits.isEmpty() ? QStringLiteral("命中的名单项：无")
                                               : QStringLiteral("命中的名单项：%1").arg(file->hits.join(QStringLiteral("、")));
    m_dialogHits->setText(hits);
    m_fileDialog->show();
    m_fileDialog->raise();
}

void HighFilterPage::openFocusedFile()
{
    const ListedFile *file = fileByRelative(m_focusRelative);
    if (!file)
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(file->absolutePath));
}

void HighFilterPage::locateFocusedFile()
{
    if (m_focusRelative.isEmpty())
        return;
    selectRelative(m_focusRelative, true);
}

} // namespace tw
