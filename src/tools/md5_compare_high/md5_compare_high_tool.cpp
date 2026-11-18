#include "tools/md5_compare_high/md5_compare_high_tool.h"

#include "shell/elided_label.h"

#include <QAction>
#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTableView>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <utility>

namespace tw {
namespace {

QLabel *makeLabel(const QString &text, const char *role)
{
    auto *label = new QLabel(text);
    label->setProperty("role", QString::fromLatin1(role));
    return label;
}

class ResultTable : public QTableView
{
public:
    explicit ResultTable(QWidget *parent = nullptr)
        : QTableView(parent)
    {
    }

    QSize sizeHint() const override { return QSize(280, 220); }
    QSize minimumSizeHint() const override { return QSize(0, 48); }
};

class PaneTree : public QTreeView
{
public:
    explicit PaneTree(QWidget *parent = nullptr)
        : QTreeView(parent)
    {
    }

    QSize sizeHint() const override { return QSize(220, 220); }
    QSize minimumSizeHint() const override { return QSize(0, 48); }
};

class ProblemList : public QPlainTextEdit
{
public:
    explicit ProblemList(QWidget *parent = nullptr)
        : QPlainTextEdit(parent)
    {
    }

    QSize sizeHint() const override { return QSize(360, 56); }
    QSize minimumSizeHint() const override { return QSize(0, 32); }
};

} // namespace

class HighResultModel : public QAbstractTableModel
{
public:
    explicit HighResultModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return m_visible.size();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return 4;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
            return {};
        const HighRow &row = m_rows.at(m_visible.at(index.row()));
        if (role == Qt::UserRole)
            return row.relativePath;
        if (role == Qt::ToolTipRole) {
            QString tip = row.relativePath + QLatin1Char('\n') + highRowKindText(row.kind);
            if (!row.md5A.isEmpty())
                tip += QStringLiteral("\nMD5 A ") + row.md5A;
            if (!row.md5B.isEmpty())
                tip += QStringLiteral("\nMD5 B ") + row.md5B;
            if (!row.note.isEmpty())
                tip += QLatin1Char('\n') + row.note;
            return tip;
        }
        if (role == Qt::FontRole && (index.column() == 2 || index.column() == 3))
            return QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (role != Qt::DisplayRole)
            return {};
        switch (index.column()) {
        case 0:
            return row.relativePath;
        case 1:
            return highRowKindText(row.kind);
        case 2:
            return row.md5A;
        case 3:
            return row.md5B;
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
            return QStringLiteral("状态");
        case 2:
            return QStringLiteral("MD5 A");
        case 3:
            return QStringLiteral("MD5 B");
        default:
            return {};
        }
    }

    void setRows(const QVector<HighRow> &rows)
    {
        beginResetModel();
        m_rows = rows;
        rebuild();
        endResetModel();
    }

    void setFilter(int kindFilter, const QString &query)
    {
        beginResetModel();
        m_kindFilter = kindFilter;
        m_query = query;
        rebuild();
        endResetModel();
    }

    QVector<HighRow> visibleRows() const
    {
        QVector<HighRow> rows;
        rows.reserve(m_visible.size());
        for (int index : m_visible)
            rows.append(m_rows.at(index));
        return rows;
    }

private:
    void rebuild()
    {
        m_visible.clear();
        m_visible.reserve(m_rows.size());
        for (int i = 0; i < m_rows.size(); ++i) {
            if (highRowVisible(m_rows.at(i), m_kindFilter, m_query))
                m_visible.append(i);
        }
    }

    QVector<HighRow> m_rows;
    QVector<int> m_visible;
    int m_kindFilter = -1;
    QString m_query;
};

class HighTreeModel : public QAbstractItemModel
{
public:
    struct Node
    {
        QString name;
        QString relativePath;
        bool file = false;
        HighRowKind kind = HighRowKind::Match;
        bool mixed = false;
        QString md5A;
        QString md5B;
        QString note;
        Node *parent = nullptr;
        QVector<Node *> children;
    };

    explicit HighTreeModel(QObject *parent = nullptr)
        : QAbstractItemModel(parent)
        , m_root(new Node)
    {
    }

    ~HighTreeModel() override { destroy(m_root); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        const Node *node = nodeAt(parent);
        return node ? node->children.size() : 0;
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        Q_UNUSED(parent);
        return 4;
    }

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override
    {
        if (!hasIndex(row, column, parent))
            return {};
        Node *node = nodeAt(parent);
        if (!node || row < 0 || row >= node->children.size())
            return {};
        return createIndex(row, column, node->children.at(row));
    }

    QModelIndex parent(const QModelIndex &child) const override
    {
        if (!child.isValid())
            return {};
        Node *node = static_cast<Node *>(child.internalPointer());
        if (!node || !node->parent || node->parent == m_root)
            return {};
        Node *parentNode = node->parent;
        const int row = parentNode->parent ? parentNode->parent->children.indexOf(parentNode) : 0;
        return createIndex(row, 0, parentNode);
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid())
            return {};
        const Node *node = static_cast<Node *>(index.internalPointer());
        if (!node)
            return {};
        if (role == Qt::UserRole)
            return node->relativePath;
        if (role == Qt::UserRole + 1)
            return node->file;
        if (role == Qt::ToolTipRole) {
            QString tip = node->relativePath;
            if (!node->file)
                tip += QStringLiteral("\n目录");
            tip += QStringLiteral("\n") + statusText(node);
            if (!node->md5A.isEmpty())
                tip += QStringLiteral("\n目录 A ") + node->md5A;
            if (!node->md5B.isEmpty())
                tip += QStringLiteral("\n目录 B ") + node->md5B;
            if (!node->note.isEmpty())
                tip += QStringLiteral("\n") + node->note;
            tip += QStringLiteral("\n单击同步对侧，双击或回车查看详情");
            return tip;
        }
        if (role == Qt::FontRole && (index.column() == 2 || index.column() == 3))
            return QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (role != Qt::DisplayRole)
            return {};
        switch (index.column()) {
        case 0:
            return node->name;
        case 1:
            return statusText(node);
        case 2:
            return node->file ? node->md5A : QString();
        case 3:
            return node->file ? node->md5B : QString();
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
            return QStringLiteral("名称");
        case 1:
            return QStringLiteral("状态");
        case 2:
            return QStringLiteral("目录 A");
        case 3:
            return QStringLiteral("目录 B");
        default:
            return {};
        }
    }

    void setRows(const QVector<HighRow> &rows)
    {
        beginResetModel();
        destroy(m_root);
        m_root = new Node;
        QHash<QString, Node *> dirs;
        dirs.insert(QString(), m_root);
        for (const HighRow &row : rows) {
            const QStringList parts = row.relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (parts.isEmpty())
                continue;
            QString prefix;
            Node *parentNode = m_root;
            for (int i = 0; i < parts.size(); ++i) {
                const bool leaf = i + 1 == parts.size();
                prefix = prefix.isEmpty() ? parts.at(i) : prefix + QLatin1Char('/') + parts.at(i);
                if (!leaf) {
                    Node *dir = dirs.value(prefix, nullptr);
                    if (!dir) {
                        dir = new Node;
                        dir->name = parts.at(i);
                        dir->relativePath = prefix;
                        dir->parent = parentNode;
                        parentNode->children.append(dir);
                        dirs.insert(prefix, dir);
                    }
                    parentNode = dir;
                } else {
                    auto *file = new Node;
                    file->name = parts.at(i);
                    file->relativePath = row.relativePath;
                    file->file = true;
                    file->kind = row.kind;
                    file->md5A = row.md5A;
                    file->md5B = row.md5B;
                    file->note = row.note;
                    file->parent = parentNode;
                    parentNode->children.append(file);
                }
            }
        }
        rollup(m_root);
        sortNode(m_root);
        endResetModel();
    }

    QModelIndex indexForPath(const QString &path) const
    {
        if (path.isEmpty() || !m_root)
            return {};
        return findPath(m_root, path);
    }

    const Node *nodeForIndex(const QModelIndex &index) const
    {
        if (!index.isValid())
            return nullptr;
        return static_cast<const Node *>(index.internalPointer());
    }

private:
    static void destroy(Node *node)
    {
        if (!node)
            return;
        for (Node *child : node->children)
            destroy(child);
        delete node;
    }

    Node *nodeAt(const QModelIndex &index) const
    {
        if (!index.isValid())
            return m_root;
        return static_cast<Node *>(index.internalPointer());
    }

    static QString statusText(const Node *node)
    {
        if (!node->file && node->mixed)
            return QStringLiteral("多种");
        return highRowKindText(node->kind);
    }

    static int severity(HighRowKind kind)
    {
        switch (kind) {
        case HighRowKind::Issue:
            return 4;
        case HighRowKind::Mismatch:
            return 3;
        case HighRowKind::OnlyA:
        case HighRowKind::OnlyB:
            return 2;
        case HighRowKind::Match:
            return 1;
        }
        return 0;
    }

    void rollup(Node *node) const
    {
        if (!node || node->file)
            return;
        bool anyFile = false;
        bool mixed = false;
        HighRowKind kind = HighRowKind::Match;
        int rank = 0;
        for (Node *child : node->children) {
            rollup(child);
            const HighRowKind childKind = child->kind;
            if (child->file || !child->children.isEmpty()) {
                if (!anyFile) {
                    kind = childKind;
                    rank = severity(childKind);
                    mixed = child->mixed;
                    anyFile = true;
                } else if (child->mixed || childKind != kind) {
                    mixed = true;
                    if (severity(childKind) > rank) {
                        kind = childKind;
                        rank = severity(childKind);
                    }
                }
            }
        }
        node->kind = kind;
        node->mixed = mixed;
    }

    void sortNode(Node *node) const
    {
        if (!node)
            return;
        std::sort(node->children.begin(), node->children.end(), [](const Node *left, const Node *right) {
            const int byName = QString::compare(left->name, right->name, Qt::CaseInsensitive);
            if (byName != 0)
                return byName < 0;
            return left->relativePath < right->relativePath;
        });
        for (Node *child : node->children)
            sortNode(child);
    }

    QModelIndex findPath(Node *node, const QString &path) const
    {
        for (int row = 0; row < node->children.size(); ++row) {
            Node *child = node->children.at(row);
            const QModelIndex idx = createIndex(row, 0, child);
            if (child->relativePath == path)
                return idx;
            if (path.startsWith(child->relativePath + QLatin1Char('/'))) {
                const QModelIndex nested = findPath(child, path);
                if (nested.isValid())
                    return nested;
            }
        }
        return {};
    }

    Node *m_root = nullptr;
};

namespace {

QWidget *directoryField(QLabel *label, QLineEdit *edit, QPushButton *browse)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    label->setBuddy(edit);
    edit->setMinimumWidth(0);
    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    browse->setAutoDefault(false);
    layout->addWidget(label);
    layout->addWidget(edit, 1);
    layout->addWidget(browse);
    return row;
}

QWidget *paneBox(const QString &title, QWidget *view, QWidget *extra = nullptr)
{
    auto *box = new QWidget;
    box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(makeLabel(title, "sectionTitle"));
    if (extra)
        layout->addWidget(extra);
    view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(view, 1);
    return box;
}

void configureTree(QTreeView *tree, const QString &name)
{
    tree->setObjectName(name);
    tree->setHeaderHidden(false);
    tree->setAllColumnsShowFocus(true);
    tree->setRootIsDecorated(true);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree->setTextElideMode(Qt::ElideMiddle);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    tree->hideColumn(2);
    tree->hideColumn(3);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    tree->header()->resizeSection(1, 108);
    tree->header()->setMinimumSectionSize(24);
    tree->setToolTip(QStringLiteral("单击同步另一侧目录树和明细表，双击或回车打开详情。"));
}

} // namespace

Md5CompareHighPage::Md5CompareHighPage(const ToolServices &services, QWidget *dialogParent, QWidget *parent)
    : QWidget(parent)
    , m_services(services)
    , m_dialogParent(dialogParent)
{
    setObjectName(QStringLiteral("md5HighPage"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(0, 0);
    setToolTip(QStringLiteral("选择两个目录后开始比较。单击结果可在两侧目录树和明细表之间跳到同一相对路径，"
                              "双击或按回车打开详情。表头可排序。"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(4);

    auto *toolbar = new QFrame;
    toolbar->setObjectName(QStringLiteral("md5HighToolbar"));
    toolbar->setProperty("role", QStringLiteral("card"));
    auto *toolbarLayout = new QVBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(6, 4, 6, 4);
    toolbarLayout->setSpacing(4);

    m_dirA = new QLineEdit;
    m_dirA->setObjectName(QStringLiteral("md5HighDirA"));
    m_dirA->setPlaceholderText(QStringLiteral("目录 A"));
    m_dirA->setAccessibleName(QStringLiteral("目录 A"));
    m_dirA->setToolTip(QStringLiteral("第一个要比较的目录"));
    m_browseA = new QPushButton(QStringLiteral("浏览"));
    m_browseA->setObjectName(QStringLiteral("md5HighBrowseA"));
    m_browseA->setAccessibleName(QStringLiteral("浏览目录 A"));
    m_dirB = new QLineEdit;
    m_dirB->setObjectName(QStringLiteral("md5HighDirB"));
    m_dirB->setPlaceholderText(QStringLiteral("目录 B"));
    m_dirB->setAccessibleName(QStringLiteral("目录 B"));
    m_dirB->setToolTip(QStringLiteral("第二个要比较的目录"));
    m_browseB = new QPushButton(QStringLiteral("浏览"));
    m_browseB->setObjectName(QStringLiteral("md5HighBrowseB"));
    m_browseB->setAccessibleName(QStringLiteral("浏览目录 B"));

    auto *dirs = new QHBoxLayout;
    dirs->setSpacing(8);
    dirs->addWidget(directoryField(makeLabel(QStringLiteral("目录 A"), "fieldLabel"), m_dirA, m_browseA), 1);
    dirs->addWidget(directoryField(makeLabel(QStringLiteral("目录 B"), "fieldLabel"), m_dirB, m_browseB), 1);
    toolbarLayout->addLayout(dirs);

    m_start = new QPushButton(QStringLiteral("开始比较"));
    m_start->setObjectName(QStringLiteral("md5HighStart"));
    m_start->setProperty("role", QStringLiteral("primary"));
    m_start->setDefault(true);
    m_start->setAutoDefault(false);
    m_start->setToolTip(QStringLiteral("在后台比较两个目录中的普通文件，比较期间锁定导航。"));
    m_cancel = new QPushButton(QStringLiteral("取消"));
    m_cancel->setObjectName(QStringLiteral("md5HighCancel"));
    m_cancel->setAutoDefault(false);
    m_cancel->setToolTip(QStringLiteral("停止比较并丢弃尚未显示的结果。"));
    m_export = new QPushButton(QStringLiteral("导出 CSV"));
    m_export->setObjectName(QStringLiteral("md5HighExport"));
    m_export->setAutoDefault(false);
    m_export->setToolTip(QStringLiteral("导出带 BOM 的 UTF-8 CSV。不能保存到正在比较的目录里。"));

    m_filter = new QComboBox;
    m_filter->setObjectName(QStringLiteral("md5HighFilter"));
    m_filter->setAccessibleName(QStringLiteral("状态过滤"));
    m_filter->setToolTip(QStringLiteral("按状态筛选两侧目录树和明细表。"));
    m_filter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_filter->setMinimumContentsLength(4);
    m_filter->addItem(QStringLiteral("全部"), -1);
    m_filter->addItem(highRowKindText(HighRowKind::Match), static_cast<int>(HighRowKind::Match));
    m_filter->addItem(highRowKindText(HighRowKind::Mismatch), static_cast<int>(HighRowKind::Mismatch));
    m_filter->addItem(highRowKindText(HighRowKind::OnlyA), static_cast<int>(HighRowKind::OnlyA));
    m_filter->addItem(highRowKindText(HighRowKind::OnlyB), static_cast<int>(HighRowKind::OnlyB));
    m_filter->addItem(highRowKindText(HighRowKind::Issue), static_cast<int>(HighRowKind::Issue));
    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("md5HighSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索"));
    m_search->setAccessibleName(QStringLiteral("路径搜索"));
    m_search->setToolTip(QStringLiteral("按相对路径搜索，忽略大小写和首尾空白。"));
    m_search->setMinimumWidth(0);
    m_search->setClearButtonEnabled(true);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(4);
    actions->addWidget(m_start);
    actions->addWidget(m_cancel);
    actions->addWidget(m_export);
    auto *filterLabel = makeLabel(QStringLiteral("状态"), "fieldLabel");
    filterLabel->setBuddy(m_filter);
    auto *searchLabel = makeLabel(QStringLiteral("路径"), "fieldLabel");
    searchLabel->setBuddy(m_search);
    actions->addWidget(filterLabel);
    actions->addWidget(m_filter);
    actions->addWidget(searchLabel);
    actions->addWidget(m_search, 1);
    toolbarLayout->addLayout(actions);

    m_state = new ElidedLabel;
    m_state->setObjectName(QStringLiteral("md5HighState"));
    m_state->setProperty("role", QStringLiteral("hint"));
    m_state->setFullText(QStringLiteral("选择两个已存在的目录后开始比较"));
    m_counts = new ElidedLabel;
    m_counts->setObjectName(QStringLiteral("md5HighCounts"));
    m_counts->setProperty("role", QStringLiteral("hint"));
    toolbarLayout->addWidget(m_state);
    toolbarLayout->addWidget(m_counts);

    m_bar = new QProgressBar;
    m_bar->setObjectName(QStringLiteral("md5HighProgress"));
    m_bar->setRange(0, 1);
    m_bar->setValue(0);
    m_bar->setTextVisible(false);
    m_bar->setAccessibleName(QStringLiteral("比较进度"));
    toolbarLayout->addWidget(m_bar);
    layout->addWidget(toolbar);

    m_model = new HighResultModel(this);
    m_treeModel = new HighTreeModel(this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setDynamicSortFilter(true);

    auto *treeA = new PaneTree;
    m_treeA = treeA;
    m_treeA->setModel(m_treeModel);
    m_treeA->setAccessibleName(QStringLiteral("目录 A 树"));
    configureTree(m_treeA, QStringLiteral("md5HighTreeA"));

    auto *treeB = new PaneTree;
    m_treeB = treeB;
    m_treeB->setModel(m_treeModel);
    m_treeB->setAccessibleName(QStringLiteral("目录 B 树"));
    configureTree(m_treeB, QStringLiteral("md5HighTreeB"));

    auto *table = new ResultTable;
    m_table = table;
    m_table->setObjectName(QStringLiteral("md5HighTable"));
    m_table->setModel(m_proxy);
    m_table->setAccessibleName(QStringLiteral("比较结果"));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideMiddle);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setMinimumSectionSize(24);
    m_table->setToolTip(QStringLiteral("单击表头排序。单击一行会定位到两侧目录树，双击打开详情。"));

    m_preview = new ElidedLabel;
    m_preview->setObjectName(QStringLiteral("md5HighPreview"));
    m_preview->setProperty("role", QStringLiteral("value"));
    m_preview->setFullText(QStringLiteral("未选择文件"));

    auto *problems = new ProblemList;
    m_problems = problems;
    m_problems->setObjectName(QStringLiteral("md5HighProblems"));
    m_problems->setReadOnly(true);
    m_problems->setAccessibleName(QStringLiteral("问题列表"));
    m_problems->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_problems->setToolTip(QStringLiteral("无法读取、特殊文件、越界或成环的符号链接记在这里。"));

    m_splitH = new QSplitter(Qt::Horizontal);
    m_splitH->setObjectName(QStringLiteral("md5HighSplitHorizontal"));
    m_splitH->setChildrenCollapsible(false);
    m_splitH->addWidget(paneBox(QStringLiteral("目录 A"), m_treeA));
    m_splitH->addWidget(paneBox(QStringLiteral("差异明细"), m_table, m_preview));
    m_splitH->addWidget(paneBox(QStringLiteral("目录 B"), m_treeB));
    m_splitH->setStretchFactor(0, 3);
    m_splitH->setStretchFactor(1, 5);
    m_splitH->setStretchFactor(2, 3);

    m_splitV = new QSplitter(Qt::Vertical);
    m_splitV->setObjectName(QStringLiteral("md5HighSplitVertical"));
    m_splitV->setChildrenCollapsible(false);
    m_splitV->addWidget(m_splitH);
    m_splitV->addWidget(paneBox(QStringLiteral("问题"), m_problems));
    m_splitV->setStretchFactor(0, 5);
    m_splitV->setStretchFactor(1, 1);
    layout->addWidget(m_splitV, 1);

    connect(m_browseA, &QPushButton::clicked, this, [this] { browse(m_dirA, QStringLiteral("选择目录 A")); });
    connect(m_browseB, &QPushButton::clicked, this, [this] { browse(m_dirB, QStringLiteral("选择目录 B")); });
    connect(m_start, &QPushButton::clicked, this, &Md5CompareHighPage::startCompare);
    connect(m_cancel, &QPushButton::clicked, this, &Md5CompareHighPage::cancelCompare);
    connect(m_export, &QPushButton::clicked, this, &Md5CompareHighPage::chooseExport);
    connect(m_dirA, &QLineEdit::editingFinished, this, &Md5CompareHighPage::saveMemory);
    connect(m_dirB, &QLineEdit::editingFinished, this, &Md5CompareHighPage::saveMemory);
    connect(m_filter, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { refilter(); });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &) { refilter(); });

    const auto followTree = [this](const QModelIndex &current) {
        if (m_syncing || !current.isValid())
            return;
        syncToPath(current.data(Qt::UserRole).toString());
    };
    connect(m_treeA->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [followTree](const QModelIndex &current, const QModelIndex &) { followTree(current); });
    connect(m_treeB->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [followTree](const QModelIndex &current, const QModelIndex &) { followTree(current); });
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current, const QModelIndex &) {
                if (m_syncing || !current.isValid())
                    return;
                syncToPath(current.data(Qt::UserRole).toString());
            });
    connect(m_treeA, &QTreeView::activated, this, [this](const QModelIndex &index) {
        openDetail(index.data(Qt::UserRole).toString());
    });
    connect(m_treeB, &QTreeView::activated, this, [this](const QModelIndex &index) {
        openDetail(index.data(Qt::UserRole).toString());
    });
    connect(m_table, &QTableView::activated, this, [this](const QModelIndex &index) {
        openDetail(index.data(Qt::UserRole).toString());
    });

    const auto popup = [this](QTreeView *tree, const QPoint &pos) {
        const QModelIndex index = tree->indexAt(pos);
        if (!index.isValid())
            return;
        QMenu menu(tree);
        QAction *detail = menu.addAction(QStringLiteral("查看详情"));
        QAction *chosen = menu.exec(tree->viewport()->mapToGlobal(pos));
        if (chosen == detail)
            openDetail(index.data(Qt::UserRole).toString());
    };
    connect(m_treeA, &QWidget::customContextMenuRequested, this, [this, popup](const QPoint &pos) {
        popup(m_treeA, pos);
    });
    connect(m_treeB, &QWidget::customContextMenuRequested, this, [this, popup](const QPoint &pos) {
        popup(m_treeB, pos);
    });

    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this, [this] { m_search->setFocus(); m_search->selectAll(); });

    setTabOrder(m_dirA, m_browseA);
    setTabOrder(m_browseA, m_dirB);
    setTabOrder(m_dirB, m_browseB);
    setTabOrder(m_browseB, m_start);
    setTabOrder(m_start, m_cancel);
    setTabOrder(m_cancel, m_export);
    setTabOrder(m_export, m_filter);
    setTabOrder(m_filter, m_search);
    setTabOrder(m_search, m_treeA);
    setTabOrder(m_treeA, m_table);
    setTabOrder(m_table, m_treeB);
    setTabOrder(m_treeB, m_problems);

    clearPresentation();
    loadMemory();
    updateActions();
}

Md5CompareHighPage::~Md5CompareHighPage()
{
    m_closing = true;
    if (m_dirA)
        disconnect(m_dirA, nullptr, this, nullptr);
    if (m_dirB)
        disconnect(m_dirB, nullptr, this, nullptr);
    stopWork();
}

void Md5CompareHighPage::stopWork()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_discardOutcome = true;
    ++m_generation;
    if (m_job.joinable())
        m_job.join();
    m_running = false;
    m_lock.release();
}

bool Md5CompareHighPage::exportReportTo(const QString &path, QString *error)
{
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        m_services.logWarning(message);
        return false;
    };
    if (m_running || !m_hasReport)
        return fail(QStringLiteral("请先完成一次比较"));
    const QString blocked = highCsvInsideTreeReason(m_rootA, m_rootB, path);
    if (!blocked.isEmpty())
        return fail(blocked);
    QString writeError;
    if (!writeHighReportCsv(path, m_report, &writeError))
        return fail(writeError.isEmpty() ? QStringLiteral("无法写入报告") : writeError);
    m_services.logSuccess(QStringLiteral("已导出比较结果"));
    if (error)
        error->clear();
    return true;
}

void Md5CompareHighPage::browse(QLineEdit *edit, const QString &title)
{
    const QString current = edit->text().trimmed();
    const QString start = QFileInfo(current).isDir() ? current : QDir::homePath();
    const QString picked = QFileDialog::getExistingDirectory(m_dialogParent ? m_dialogParent : this, title, start);
    if (picked.isEmpty())
        return;
    edit->setText(QDir::toNativeSeparators(picked));
    saveMemory();
}

void Md5CompareHighPage::startCompare()
{
    if (m_running)
        return;
    if (m_job.joinable())
        m_job.join();

    const QString dirA = m_dirA->text().trimmed();
    const QString dirB = m_dirB->text().trimmed();
    saveMemory();
    if (dirA.isEmpty() || dirB.isEmpty()) {
        m_state->setFullText(QStringLiteral("请选择两个目录"));
        m_services.showStatus(StatusState::Error, QStringLiteral("请选择两个目录"));
        return;
    }

    clearPresentation();
    m_rootA = dirA;
    m_rootB = dirB;
    m_stopRequested.store(false, std::memory_order_release);
    m_discardOutcome = false;
    ++m_generation;
    const quint64 generation = m_generation;
    m_running = true;
    m_lock = m_services.lockNavigation(QStringLiteral("正在比较目录 MD5"));
    m_services.logInfo(QStringLiteral("开始比较目录"));
    m_services.showStatus(StatusState::Busy, QStringLiteral("正在比较目录 MD5"));
    m_state->setFullText(QStringLiteral("正在比较目录 MD5"));
    m_bar->setRange(0, 0);
    updateActions();

    m_job = std::thread([this, generation, dirA, dirB] {
        HighReport report;
        QString error;
        bool ok = false;
        try {
            auto last = std::chrono::steady_clock::time_point::min();
            bool sawHash = false;
            const HighProgressFn publish = [this, generation, &last, &sawHash](const HighProgress &progress) {
                const bool enteredHash = !progress.scanning && !sawHash;
                if (!progress.scanning)
                    sawHash = true;
                const auto now = std::chrono::steady_clock::now();
                const bool first = last == std::chrono::steady_clock::time_point::min();
                if (!first && !enteredHash && now - last < std::chrono::milliseconds(150))
                    return;
                last = now;
                const HighProgress copy = progress;
                QMetaObject::invokeMethod(
                    this,
                    [this, generation, copy] {
                        if (generation != m_generation || !m_running)
                            return;
                        applyProgress(copy);
                    },
                    Qt::QueuedConnection);
            };
            ok = digestDirectoryPair(
                dirA, dirB, &report, &error, publish,
                [this] { return m_stopRequested.load(std::memory_order_acquire); });
        } catch (const std::exception &ex) {
            ok = false;
            error = QString::fromUtf8(ex.what());
            report = HighReport();
        } catch (...) {
            ok = false;
            error = QStringLiteral("比较过程中出现未预期的错误");
            report = HighReport();
        }
        QMetaObject::invokeMethod(
            this,
            [this, generation, ok, report = std::move(report), error = std::move(error)] {
                finishCompare(generation, ok, report, error);
            },
            Qt::QueuedConnection);
    });
}

void Md5CompareHighPage::cancelCompare()
{
    if (!m_running)
        return;
    m_discardOutcome = true;
    m_stopRequested.store(true, std::memory_order_release);
    ++m_generation;
    if (m_job.joinable())
        m_job.join();
    m_running = false;
    m_lock.release();
    clearPresentation();
    m_state->setFullText(QStringLiteral("比较已取消"));
    m_services.logWarning(QStringLiteral("比较已取消"));
    m_services.showStatus(StatusState::Info, QStringLiteral("比较已取消"));
    updateActions();
}

void Md5CompareHighPage::chooseExport()
{
    const QString picked = QFileDialog::getSaveFileName(m_dialogParent ? m_dialogParent : this,
                                                        QStringLiteral("导出比较结果"), QString(),
                                                        QStringLiteral("CSV 文件 (*.csv)"));
    if (picked.isEmpty())
        return;
    QString error;
    if (!exportReportTo(picked, &error))
        m_state->setFullText(error);
    else
        m_state->setFullText(QStringLiteral("已导出比较结果"));
}

void Md5CompareHighPage::finishCompare(quint64 generation, bool ok, const HighReport &report, const QString &error)
{
    if (m_job.joinable())
        m_job.join();
    if (m_closing || generation != m_generation)
        return;

    const bool discard = m_discardOutcome || report.cancelled;
    m_discardOutcome = false;
    m_running = false;
    m_lock.release();
    if (discard) {
        clearPresentation();
        m_state->setFullText(QStringLiteral("比较已取消"));
        m_services.logWarning(QStringLiteral("比较已取消"));
        m_services.showStatus(StatusState::Info, QStringLiteral("比较已取消"));
        updateActions();
        return;
    }
    if (!ok) {
        clearPresentation();
        const QString message = error.isEmpty() ? QStringLiteral("比较失败") : error;
        m_state->setFullText(message);
        m_services.logError(message);
        m_services.showStatus(StatusState::Error, message);
        updateActions();
        return;
    }

    showReport(report);
    m_services.logSuccess(QStringLiteral("比较完成：相同 %1，内容不同 %2，仅在目录 A %3，仅在目录 B %4，问题 %5")
                              .arg(report.matchCount)
                              .arg(report.mismatchCount)
                              .arg(report.onlyACount)
                              .arg(report.onlyBCount)
                              .arg(report.issueCount));
    if (report.reachedCap)
        m_services.logWarning(QStringLiteral("已达到扫描上限，其余内容未继续比较"));
    m_services.showStatus(StatusState::Success, QStringLiteral("比较完成"));
    updateActions();
}

void Md5CompareHighPage::applyProgress(const HighProgress &progress)
{
    if (!m_running)
        return;
    if (progress.scanning || progress.total <= 0) {
        m_bar->setRange(0, 0);
    } else {
        m_bar->setRange(0, progress.total);
        m_bar->setValue(qBound(0, progress.done, progress.total));
    }
    QString text = progress.scanning ? QStringLiteral("正在扫描") : QStringLiteral("正在计算 MD5");
    if (!progress.path.isEmpty())
        text += QLatin1Char(' ') + progress.path;
    m_state->setFullText(text);
}

void Md5CompareHighPage::showReport(const HighReport &report)
{
    m_report = report;
    m_hasReport = true;
    m_model->setRows(report.rows);
    m_counts->setFullText(QStringLiteral("相同 %1 · 内容不同 %2 · 仅在目录 A %3 · 仅在目录 B %4 · 问题 %5")
                          .arg(report.matchCount)
                          .arg(report.mismatchCount)
                          .arg(report.onlyACount)
                          .arg(report.onlyBCount)
                          .arg(report.issueCount));
    if (report.issues.isEmpty()) {
        m_problems->setPlainText(QStringLiteral("没有问题"));
    } else {
        QStringList lines;
        lines.reserve(report.issues.size());
        for (const HighIssue &issue : report.issues) {
            const QString where = issue.relativePath.isEmpty() ? QStringLiteral("（扫描）") : issue.relativePath;
            const QString side = issue.side == QLatin1String("B") ? QStringLiteral("目录 B") : QStringLiteral("目录 A");
            lines.append(side + QStringLiteral(" / ") + where + QStringLiteral("：") + issue.message);
        }
        m_problems->setPlainText(lines.join(QLatin1Char('\n')));
    }
    refilter();
    m_state->setFullText(report.reachedCap ? QStringLiteral("比较完成，已达到扫描上限")
                                           : QStringLiteral("比较完成"));
    m_bar->setRange(0, 1);
    m_bar->setValue(1);
}

void Md5CompareHighPage::clearPresentation()
{
    m_report = HighReport();
    m_hasReport = false;
    m_rootA.clear();
    m_rootB.clear();
    if (m_model)
        m_model->setRows({});
    if (m_treeModel)
        m_treeModel->setRows({});
    if (m_detail)
        m_detail->hide();
    if (m_preview)
        m_preview->setFullText(QStringLiteral("未选择文件"));
    if (m_counts) {
        m_counts->setFullText(QStringLiteral("相同 %1 · 内容不同 %2 · 仅在目录 A %3 · 仅在目录 B %4 · 问题 %5")
                              .arg(0)
                              .arg(0)
                              .arg(0)
                              .arg(0)
                              .arg(0));
    }
    if (m_problems)
        m_problems->setPlainText(QStringLiteral("没有问题"));
    if (m_bar) {
        m_bar->setRange(0, 1);
        m_bar->setValue(0);
    }
    if (m_filter && m_search && m_model)
        refilter();
}

void Md5CompareHighPage::refilter()
{
    int kindFilter = -1;
    if (m_filter->currentData().isValid())
        kindFilter = m_filter->currentData().toInt();
    m_model->setFilter(kindFilter, m_search->text());
    if (!m_treeModel)
        return;
    const QVector<HighRow> visible = m_model->visibleRows();
    m_treeModel->setRows(visible);
    if (visible.size() <= 400) {
        m_treeA->expandAll();
        m_treeB->expandAll();
    }
}

void Md5CompareHighPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    applySplitterSizes();
}

void Md5CompareHighPage::applySplitterSizes()
{
    if (m_splitSized || !m_splitV || !m_splitH || height() < 160)
        return;
    m_splitSized = true;
    const int log = qBound(72, height() / 5, 160);
    m_splitV->setSizes({qMax(1, height() - log), log});
    const int width = qMax(1, m_splitH->width());
    m_splitH->setSizes({width * 3, width * 5, width * 3});
}

void Md5CompareHighPage::syncToPath(const QString &relativePath)
{
    if (m_syncing || relativePath.isEmpty())
        return;
    m_syncing = true;
    revealPath(m_treeA, relativePath);
    revealPath(m_treeB, relativePath);
    revealInTable(relativePath);
    const QModelIndex treeIndex = m_treeModel->indexForPath(relativePath);
    const HighTreeModel::Node *node = m_treeModel->nodeForIndex(treeIndex);
    if (m_preview) {
        if (!node) {
            m_preview->setFullText(relativePath);
        } else if (node->file) {
            m_preview->setFullText(relativePath + QStringLiteral("  ·  ") + highRowKindText(node->kind));
        } else {
            const QString status = node->mixed ? QStringLiteral("多种") : highRowKindText(node->kind);
            m_preview->setFullText(QStringLiteral("目录  ·  ") + relativePath + QStringLiteral("  ·  ") + status);
        }
    }
    if (m_detail && m_detail->isVisible())
        fillDetail(relativePath);
    m_syncing = false;
}

void Md5CompareHighPage::revealPath(QTreeView *tree, const QString &relativePath)
{
    const QModelIndex index = m_treeModel->indexForPath(relativePath);
    if (!index.isValid()) {
        tree->setCurrentIndex({});
        return;
    }
    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent())
        tree->expand(parent);
    tree->setCurrentIndex(index);
    tree->scrollTo(index, QAbstractItemView::PositionAtCenter);
}

void Md5CompareHighPage::revealInTable(const QString &relativePath)
{
    if (!m_proxy)
        return;
    for (int row = 0; row < m_proxy->rowCount(); ++row) {
        const QModelIndex index = m_proxy->index(row, 0);
        if (index.data(Qt::UserRole).toString() != relativePath)
            continue;
        m_table->setCurrentIndex(index);
        m_table->scrollTo(index, QAbstractItemView::PositionAtCenter);
        return;
    }
    m_table->setCurrentIndex({});
}

void Md5CompareHighPage::openDetail(const QString &relativePath)
{
    if (relativePath.isEmpty())
        return;
    if (!m_detail) {
        m_detail = new QDialog(this);
        m_detail->setObjectName(QStringLiteral("md5HighDetail"));
        m_detail->setWindowTitle(QStringLiteral("比较详情"));
        m_detail->setModal(false);
        auto *form = new QFormLayout(m_detail);
        form->setContentsMargins(12, 12, 12, 12);
        const auto addValue = [this, form](const QString &label, QLabel **target, const QString &name) {
            auto *value = new QLabel;
            value->setObjectName(name);
            value->setProperty("role", QStringLiteral("value"));
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            value->setWordWrap(true);
            form->addRow(makeLabel(label, "fieldLabel"), value);
            *target = value;
        };
        addValue(QStringLiteral("相对路径"), &m_detailPath, QStringLiteral("md5HighDetailPath"));
        addValue(QStringLiteral("类型"), &m_detailKind, QStringLiteral("md5HighDetailKind"));
        addValue(QStringLiteral("状态"), &m_detailStatus, QStringLiteral("md5HighDetailStatus"));
        addValue(QStringLiteral("目录 A 的 MD5"), &m_detailMd5A, QStringLiteral("md5HighDetailMd5A"));
        addValue(QStringLiteral("目录 B 的 MD5"), &m_detailMd5B, QStringLiteral("md5HighDetailMd5B"));
        addValue(QStringLiteral("说明"), &m_detailNote, QStringLiteral("md5HighDetailNote"));
        addValue(QStringLiteral("目录 A 绝对路径"), &m_detailAbsA, QStringLiteral("md5HighDetailAbsA"));
        addValue(QStringLiteral("目录 B 绝对路径"), &m_detailAbsB, QStringLiteral("md5HighDetailAbsB"));
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        auto *copy = buttons->addButton(QStringLiteral("复制 MD5"), QDialogButtonBox::ActionRole);
        connect(copy, &QPushButton::clicked, this, [this] {
            const QString text = QStringLiteral("目录 A\t%1\n目录 B\t%2")
                                     .arg(m_detailMd5A ? m_detailMd5A->text() : QString(),
                                          m_detailMd5B ? m_detailMd5B->text() : QString());
            if (QClipboard *clipboard = QGuiApplication::clipboard())
                clipboard->setText(text);
        });
        connect(buttons, &QDialogButtonBox::rejected, m_detail, &QDialog::hide);
        form->addRow(buttons);
        m_detail->resize(560, 320);
    }
    fillDetail(relativePath);
    m_detail->show();
    m_detail->raise();
    syncToPath(relativePath);
}

void Md5CompareHighPage::fillDetail(const QString &relativePath)
{
    if (!m_detailPath)
        return;
    const QModelIndex index = m_treeModel ? m_treeModel->indexForPath(relativePath) : QModelIndex();
    const HighTreeModel::Node *node = m_treeModel ? m_treeModel->nodeForIndex(index) : nullptr;
    const HighRow *row = nullptr;
    for (const HighRow &candidate : m_report.rows) {
        if (candidate.relativePath == relativePath) {
            row = &candidate;
            break;
        }
    }
    m_detailPath->setText(relativePath);
    const bool file = node ? node->file : row != nullptr;
    m_detailKind->setText(file ? QStringLiteral("文件") : QStringLiteral("目录"));
    QString status;
    if (node && !node->file && node->mixed)
        status = QStringLiteral("多种");
    else if (row)
        status = highRowKindText(row->kind);
    else if (node)
        status = highRowKindText(node->kind);
    m_detailStatus->setText(status);
    m_detailMd5A->setText(row ? row->md5A : QString());
    m_detailMd5B->setText(row ? row->md5B : QString());
    m_detailNote->setText(row && !row->note.isEmpty() ? row->note : QStringLiteral("无"));
    const auto absolute = [](const QString &root, const QString &relative) {
        if (root.isEmpty())
            return relative;
        return QDir(root).filePath(relative);
    };
    m_detailAbsA->setText(absolute(m_rootA, relativePath));
    m_detailAbsB->setText(absolute(m_rootB, relativePath));
}

void Md5CompareHighPage::updateActions()
{
    m_start->setEnabled(!m_running);
    m_cancel->setEnabled(m_running);
    m_export->setEnabled(!m_running && m_hasReport);
    m_dirA->setEnabled(!m_running);
    m_dirB->setEnabled(!m_running);
    m_browseA->setEnabled(!m_running);
    m_browseB->setEnabled(!m_running);
}

QString Md5CompareHighPage::memoryFile() const
{
    QString error;
    const QString dir = m_services.dataDir(&error);
    if (dir.isEmpty())
        return {};
    return QDir(dir).filePath(QStringLiteral("watched-folders.json"));
}

void Md5CompareHighPage::loadMemory()
{
    const QString path = memoryFile();
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return;
    const QJsonObject obj = doc.object();
    m_dirA->setText(obj.value(QStringLiteral("directoryA")).toString());
    m_dirB->setText(obj.value(QStringLiteral("directoryB")).toString());
}

void Md5CompareHighPage::saveMemory()
{
    if (m_closing || !m_dirA || !m_dirB)
        return;
    const QString path = memoryFile();
    if (path.isEmpty())
        return;
    QJsonObject obj;
    obj.insert(QStringLiteral("directoryA"), m_dirA->text());
    obj.insert(QStringLiteral("directoryB"), m_dirB->text());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        m_services.logWarning(QStringLiteral("无法保存目录记录"));
        return;
    }
    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size() || !file.commit())
        m_services.logWarning(QStringLiteral("无法保存目录记录"));
}

ToolInfo Md5CompareHighTool::info() const
{
    ToolInfo described;
    described.id = QStringLiteral("md5-compare-high");
    described.title = QStringLiteral("目录 MD5 比较high");
    described.summary = QStringLiteral("按相对路径递归比较两个目录中普通文件的 MD5");
    described.iconKey = QStringLiteral("status");
    described.order = 42;
    return described;
}

bool Md5CompareHighTool::initialize(const ToolContext &context, QString *error)
{
    m_context = context;
    QString dirError;
    if (m_context.services.dataDir(&dirError).isEmpty()) {
        if (error)
            *error = dirError;
        return false;
    }
    if (error)
        error->clear();
    return true;
}

QVector<ToolPage> Md5CompareHighTool::createPages()
{
    m_page = new Md5CompareHighPage(m_context.services, m_context.dialogParent);
    ToolPage page;
    page.info.id = QStringLiteral("digest");
    page.info.title = QStringLiteral("比较");
    page.info.summary = QStringLiteral("按相对路径比较两个目录");
    page.info.iconKey = QStringLiteral("status");
    page.widget = m_page;
    QVector<ToolPage> pages;
    pages.append(page);
    return pages;
}

void Md5CompareHighTool::shutdown()
{
    if (m_page)
        m_page->stopWork();
}

} // namespace tw
