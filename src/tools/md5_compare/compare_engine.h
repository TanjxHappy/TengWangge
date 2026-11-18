#pragma once

#include <QString>
#include <QVector>

#include <functional>

namespace tw {

// 两侧普通文件按相对路径对齐后的比较状态。
// 读不出来、符号链接越界或特殊文件不记入前四类，而是记为问题。
enum class CompareStatus {
    Same,
    Different,
    OnlyInA,
    OnlyInB,
    Problem,
};

enum class FileHashStatus { Ok, Failed, Cancelled };

struct CompareEntry {
    QString relativePath;
    CompareStatus status = CompareStatus::Problem;
    QString md5A;
    QString md5B;
    QString detail;
};

struct CompareProblem {
    QString side; // "A" 或 "B"
    QString relativePath;
    QString message;
};

struct CompareResult {
    QVector<CompareEntry> entries;
    QVector<CompareProblem> problems;
    bool cancelled = false;
    bool truncated = false;
    int sameCount = 0;
    int differentCount = 0;
    int onlyInACount = 0;
    int onlyInBCount = 0;
    // 表格里状态为「问题」的行数。同一相对路径的多条说明只计一行。
    int problemCount = 0;
};

struct CompareProgress {
    bool enumerating = false;
    int completed = 0;
    int total = 0;
    QString currentPath;
};

// maxDepth 含起始目录本身：1 表示只扫描起始目录，不进入子目录。
struct CompareLimits {
    int maxDepth = 128;
    int maxFiles = 500000;
    int maxEntries = 500000;
};

using CompareProgressCallback = std::function<void(const CompareProgress &)>;
using CompareCancelCallback = std::function<bool()>;

QString compareStatusLabel(CompareStatus status);

// statusFilter < 0 表示全部状态。query 按相对路径子串匹配，忽略大小写；空白视为不过滤。
bool compareEntryMatches(const CompareEntry &entry, int statusFilter, const QString &query);

// 只读计算 path 的 MD5。rootCanonical 是起始目录的规范路径。
// 符号链接会先解析：目标必须是起始目录之内的普通文件，否则失败且不打开。
// 取消时不保留半成品摘要。
FileHashStatus hashFileMd5(const QString &absolutePath, const QString &rootCanonical, QString *hex,
                           QString *error, const CompareCancelCallback &shouldCancel = {});

// 递归比较两个已存在的目录。成功、取消都返回 true；目录本身不可用时返回 false。
// 取消时 result->cancelled 为 true，且不保留不完整的条目。
// 只读取两侧目录，不会创建或修改其中的文件。
bool compareDirectories(const QString &dirA, const QString &dirB, CompareResult *result, QString *error,
                        const CompareProgressCallback &progress = {},
                        const CompareCancelCallback &shouldCancel = {}, CompareLimits limits = {});

// 把结果写成 UTF-8 CSV（带 BOM，字段一律加引号）。按行写入，不把整份报告攒在内存里。
// 取消时不留下半成品文件。调用方负责不要把路径指进被比较的目录。
bool writeCompareReport(const QString &filePath, const CompareResult &result, QString *error,
                        const CompareCancelCallback &shouldCancel = {});

// 导出路径落在任一被比较目录（或其符号链接）之内时返回原因，否则返回空串。
QString compareReportBlockReason(const QString &dirA, const QString &dirB, const QString &reportPath);

} // namespace tw
