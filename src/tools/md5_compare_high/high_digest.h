#pragma once

#include <QString>
#include <QVector>

#include <functional>

namespace tw {

// 按相对路径对齐后的一行比较结果。读失败、特殊文件、越界或成环的符号链接
// 不计入前四类，而是记为 Issue，并同时出现在 HighReport::issues 里。
enum class HighRowKind {
    Match = 0,
    Mismatch = 1,
    OnlyA = 2,
    OnlyB = 3,
    Issue = 4,
};

enum class FileMd5State { Ready, Failed, Stopped };

struct HighRow {
    QString relativePath;
    HighRowKind kind = HighRowKind::Issue;
    QString md5A;
    QString md5B;
    QString note;
};

struct HighIssue {
    QString side; // "A" 或 "B"
    QString relativePath;
    QString message;
};

struct HighReport {
    QVector<HighRow> rows;
    QVector<HighIssue> issues;
    bool cancelled = false;
    bool reachedCap = false;
    int matchCount = 0;
    int mismatchCount = 0;
    int onlyACount = 0;
    int onlyBCount = 0;
    int issueCount = 0;
};

struct HighProgress {
    bool scanning = false;
    int done = 0;
    int total = 0;
    QString path;
};

// depth 含起始目录本身：1 表示只看起始目录这一层。
// files / items 是单侧上限，达到后停止该侧剩余扫描并在 issues 里说明。
struct HighCaps {
    int depth = 128;
    int files = 500000;
    int items = 500000;
};

using HighProgressFn = std::function<void(const HighProgress &)>;
using HighStopFn = std::function<bool()>;

QString highRowKindText(HighRowKind kind);

// kindFilter < 0 表示全部。query 去掉首尾空白后按相对路径子串匹配，忽略大小写。
bool highRowVisible(const HighRow &row, int kindFilter, const QString &query);

// 只读计算 absolutePath 的 MD5。rootCanonical 是起始目录的规范路径。
// 符号链接会先解析：最终目标必须是起始目录之内的普通文件，否则失败且不打开。
// 取消时不返回半成品摘要。
FileMd5State hashContainedFileMd5(const QString &absolutePath, const QString &rootCanonical,
                                  QString *hex, QString *error,
                                  const HighStopFn &stopRequested = {});

// 递归比较两个已存在的目录。成功和取消都返回 true；目录本身不可用时返回 false。
// 取消时 report->cancelled 为 true，且不保留已算出的行。
// 只读取两侧目录，不创建、不修改其中的文件。
bool digestDirectoryPair(const QString &dirA, const QString &dirB, HighReport *report, QString *error,
                         const HighProgressFn &progress = {}, const HighStopFn &stopRequested = {},
                         HighCaps caps = {});

// 写成带 BOM 的 UTF-8 CSV，字段一律加引号。调用方要先确认路径不在被比较目录内。
bool writeHighReportCsv(const QString &filePath, const HighReport &report, QString *error);

// 导出路径（含尚未创建的文件、以及指向目录内部的符号链接）落在任一被比较目录内时，
// 返回拒绝原因；可以导出时返回空串。
QString highCsvInsideTreeReason(const QString &dirA, const QString &dirB, const QString &csvPath);

} // namespace tw
