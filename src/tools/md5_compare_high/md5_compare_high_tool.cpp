#include "tools/md5_compare_high/md5_compare_high_tool.h"

#include "shell/elided_label.h"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSizePolicy>
#include <QTableView>
#include <QVBoxLayout>

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

    QSize sizeHint() const override { return QSize(360, 160); }
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
        if (role == Qt::ToolTipRole)
            return row.note.isEmpty() ? QVariant(row.relativePath)
                                       : QVariant(row.relativePath + QLatin1Char('\n') + row.note);
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
            return QStringLiteral("目录 A 的 MD5");
        case 3:
            return QStringLiteral("目录 B 的 MD5");
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

namespace {

QWidget *directoryRow(QLabel *label, QLineEdit *edit, QPushButton *browse)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    label->setBuddy(edit);
    edit->setMinimumWidth(0);
    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(label);
    layout->addWidget(edit, 1);
    layout->addWidget(browse);
    return row;
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

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);

    layout->addWidget(makeLabel(QStringLiteral("目录 MD5 比较high"), "pageTitle"));
    auto *subtitle = new ElidedLabel;
    subtitle->setProperty("role", QStringLiteral("pageSubtitle"));
    subtitle->setFullText(QStringLiteral("选择两个目录，按相对路径比较其中普通文件的 MD5。"));
    layout->addWidget(subtitle);

    auto *card = new QFrame;
    card->setProperty("role", QStringLiteral("card"));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(8, 6, 8, 6);
    cardLayout->setSpacing(4);

    m_dirA = new QLineEdit;
    m_dirA->setObjectName(QStringLiteral("md5HighDirA"));
    m_dirA->setPlaceholderText(QStringLiteral("第一个目录"));
    m_dirA->setAccessibleName(QStringLiteral("目录 A"));
    m_dirA->setClearButtonEnabled(false);
    m_browseA = new QPushButton(QStringLiteral("浏览"));
    m_browseA->setObjectName(QStringLiteral("md5HighBrowseA"));
    m_browseA->setAccessibleName(QStringLiteral("浏览目录 A"));
    cardLayout->addWidget(directoryRow(makeLabel(QStringLiteral("目录 A"), "fieldLabel"), m_dirA, m_browseA));

    m_dirB = new QLineEdit;
    m_dirB->setObjectName(QStringLiteral("md5HighDirB"));
    m_dirB->setPlaceholderText(QStringLiteral("第二个目录"));
    m_dirB->setAccessibleName(QStringLiteral("目录 B"));
    m_browseB = new QPushButton(QStringLiteral("浏览"));
    m_browseB->setObjectName(QStringLiteral("md5HighBrowseB"));
    m_browseB->setAccessibleName(QStringLiteral("浏览目录 B"));
    cardLayout->addWidget(directoryRow(makeLabel(QStringLiteral("目录 B"), "fieldLabel"), m_dirB, m_browseB));

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    m_start = new QPushButton(QStringLiteral("开始比较"));
    m_start->setObjectName(QStringLiteral("md5HighStart"));
    m_start->setProperty("role", QStringLiteral("primary"));
    m_start->setDefault(true);
    m_cancel = new QPushButton(QStringLiteral("取消"));
    m_cancel->setObjectName(QStringLiteral("md5HighCancel"));
    m_export = new QPushButton(QStringLiteral("导出 CSV"));
    m_export->setObjectName(QStringLiteral("md5HighExport"));
    buttons->addWidget(m_start);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_export);
    buttons->addStretch(1);
    cardLayout->addLayout(buttons);
    layout->addWidget(card);

    m_bar = new QProgressBar;
    m_bar->setObjectName(QStringLiteral("md5HighProgress"));
    m_bar->setRange(0, 1);
    m_bar->setValue(0);
    m_bar->setTextVisible(false);
    m_bar->setAccessibleName(QStringLiteral("比较进度"));
    layout->addWidget(m_bar);

    m_state = new ElidedLabel;
    m_state->setObjectName(QStringLiteral("md5HighState"));
    m_state->setProperty("role", QStringLiteral("value"));
    m_state->setFullText(QStringLiteral("选择两个已存在的目录后开始比较"));
    layout->addWidget(m_state);

    m_counts = makeLabel(QString(), "hint");
    m_counts->setObjectName(QStringLiteral("md5HighCounts"));
    m_counts->setWordWrap(true);
    layout->addWidget(m_counts);

    auto *filters = new QHBoxLayout;
    filters->setSpacing(6);
    m_filter = new QComboBox;
    m_filter->setObjectName(QStringLiteral("md5HighFilter"));
    m_filter->setAccessibleName(QStringLiteral("状态过滤"));
    m_filter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_filter->setMinimumContentsLength(6);
    m_filter->addItem(QStringLiteral("全部"), -1);
    m_filter->addItem(highRowKindText(HighRowKind::Match), static_cast<int>(HighRowKind::Match));
    m_filter->addItem(highRowKindText(HighRowKind::Mismatch), static_cast<int>(HighRowKind::Mismatch));
    m_filter->addItem(highRowKindText(HighRowKind::OnlyA), static_cast<int>(HighRowKind::OnlyA));
    m_filter->addItem(highRowKindText(HighRowKind::OnlyB), static_cast<int>(HighRowKind::OnlyB));
    m_filter->addItem(highRowKindText(HighRowKind::Issue), static_cast<int>(HighRowKind::Issue));
    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("md5HighSearch"));
    m_search->setPlaceholderText(QStringLiteral("按相对路径搜索"));
    m_search->setAccessibleName(QStringLiteral("路径搜索"));
    m_search->setMinimumWidth(0);
    m_search->setClearButtonEnabled(true);
    auto *filterLabel = makeLabel(QStringLiteral("状态"), "fieldLabel");
    filterLabel->setBuddy(m_filter);
    auto *searchLabel = makeLabel(QStringLiteral("路径"), "fieldLabel");
    searchLabel->setBuddy(m_search);
    filters->addWidget(filterLabel);
    filters->addWidget(m_filter);
    filters->addWidget(searchLabel);
    filters->addWidget(m_search, 1);
    layout->addLayout(filters);

    m_model = new HighResultModel(this);
    auto *table = new ResultTable;
    m_table = table;
    m_table->setObjectName(QStringLiteral("md5HighTable"));
    m_table->setModel(m_model);
    m_table->setAccessibleName(QStringLiteral("比较结果"));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideMiddle);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setMinimumSectionSize(24);
    m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(m_table, 1);

    layout->addWidget(makeLabel(QStringLiteral("问题"), "sectionTitle"));
    auto *problems = new ProblemList;
    m_problems = problems;
    m_problems->setObjectName(QStringLiteral("md5HighProblems"));
    m_problems->setReadOnly(true);
    m_problems->setAccessibleName(QStringLiteral("问题列表"));
    m_problems->setMaximumHeight(72);
    m_problems->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(m_problems);

    connect(m_browseA, &QPushButton::clicked, this, [this] { browse(m_dirA, QStringLiteral("选择目录 A")); });
    connect(m_browseB, &QPushButton::clicked, this, [this] { browse(m_dirB, QStringLiteral("选择目录 B")); });
    connect(m_start, &QPushButton::clicked, this, &Md5CompareHighPage::startCompare);
    connect(m_cancel, &QPushButton::clicked, this, &Md5CompareHighPage::cancelCompare);
    connect(m_export, &QPushButton::clicked, this, &Md5CompareHighPage::chooseExport);
    connect(m_dirA, &QLineEdit::editingFinished, this, &Md5CompareHighPage::saveMemory);
    connect(m_dirB, &QLineEdit::editingFinished, this, &Md5CompareHighPage::saveMemory);
    connect(m_filter, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { refilter(); });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &) { refilter(); });

    setTabOrder(m_dirA, m_browseA);
    setTabOrder(m_browseA, m_dirB);
    setTabOrder(m_dirB, m_browseB);
    setTabOrder(m_browseB, m_start);
    setTabOrder(m_start, m_cancel);
    setTabOrder(m_cancel, m_export);
    setTabOrder(m_export, m_filter);
    setTabOrder(m_filter, m_search);
    setTabOrder(m_search, m_table);
    setTabOrder(m_table, m_problems);

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
    m_counts->setText(QStringLiteral("相同 %1 · 内容不同 %2 · 仅在目录 A %3 · 仅在目录 B %4 · 问题 %5")
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
    if (m_counts) {
        m_counts->setText(QStringLiteral("相同 %1 · 内容不同 %2 · 仅在目录 A %3 · 仅在目录 B %4 · 问题 %5")
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
