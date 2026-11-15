#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

namespace tw {

// 一条扫描到的文件。匹配只看相对路径和文件名，不看文件内容，
// 也不把所选目录本身的绝对路径算进去。
struct FilterFileRow
{
    QString absolutePath;
    QString relativePath;
    QString fileName;
    QStringList matchedTerms;
    bool matched = false;
};

struct FilterProblem
{
    QString path;
    QString reason;
};

struct FilterScanResult
{
    QString rootPath;
    QVector<FilterFileRow> files;
    QVector<FilterProblem> problems;
    bool cancelled = false;
    QString error;
};

struct FilterDeleteItem
{
    QString absolutePath;
    bool removed = false;
    QString error;
};

struct FilterDeleteResult
{
    QVector<FilterDeleteItem> items;
    bool cancelled = false;
    QString error;
};

// 每行一个关键词。去掉首尾空白，忽略空行，按大小写不敏感去重并保留第一次的写法。
QStringList parseKeywords(const QString &text);

// 关键词是子串，比较前做 Unicode case fold。相对路径或文件名任一命中即算。
QStringList matchedKeywords(const QStringList &keywords, const QString &relativePath,
                            const QString &fileName);

// 递归扫描 rootPath 下的普通文件和指向文件的符号链接。
// 子目录用 openat(O_NOFOLLOW) 打开，不进入指向起始目录之外的链接，
// 不用目录符号链接展开出第二套路径，并用 inode 挡住循环。
// progress 在调用 scanDirectory 的线程上执行，可以为空；不要在其中直接改界面。
FilterScanResult scanDirectory(const QString &rootPath, const QStringList &keywords,
                               const std::atomic<bool> &cancel,
                               const std::function<void(int files, int problems)> &progress);

// 只删除 rootPath 之内的普通文件或符号链接本身。目录、越出根目录的路径、
// 以及中间经过符号链接的路径都会拒绝，并在对应条目里写明原因。
FilterDeleteResult deleteListedFiles(const QString &rootPath, const QStringList &absolutePaths,
                                     const std::atomic<bool> &cancel,
                                     const std::function<void(int done, int total)> &progress);

} // namespace tw
