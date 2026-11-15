#pragma once

#include "tools/filter_unique_high/term_list.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

namespace tw {

// 扫描到的一个普通文件，或一个指向普通文件的符号链接。
// 匹配只用相对路径和链接自己的文件名，不读文件内容，也不把起始目录的绝对路径算进去。
struct ListedFile
{
    QString absolutePath;
    QString relativePath;
    QString fileName;
    QStringList hits;
    bool matched = false;
};

struct TreeIssue
{
    QString path;
    QString reason;
};

struct TreeWalk
{
    QString rootPath;
    QVector<ListedFile> files;
    QVector<TreeIssue> issues;
    bool cancelled = false;
    QString error;
};

struct RemovalNote
{
    QString absolutePath;
    bool removed = false;
    QString error;
};

struct RemovalBatch
{
    QVector<RemovalNote> items;
    bool cancelled = false;
    QString error;
};

// 空字符串表示这个绝对路径可以作为扫描起点。否则是给用户看的拒绝原因。
// 可用时 usablePath 写成去掉多余斜杠后的绝对路径。
QString rootRefusal(const QString &path, QString *usablePath = nullptr);

// 递归列出 rootPath 里的普通文件和指向普通文件的符号链接。
// 目录符号链接不进入。指向起始目录之外的目录链接、循环链接、打不开的项记入 issues。
// 进度回调在调用 walkTree 的线程上执行，可以为空。paceMs 用来把长任务拉开，便于取消。
TreeWalk walkTree(const QString &rootPath, const QVector<Term> &terms,
                  const std::atomic<bool> &cancel, int paceMs,
                  const std::function<void(int files, int issues)> &progress);

// 只删除 rootPath 之内的普通文件或符号链接本身。
// 目录、越出根目录的路径、带 .. 的路径、中间经过符号链接的路径都会拒绝，并写明原因。
// 指向外部文件的符号链接被删掉时，目标文件还在。
RemovalBatch removeListed(const QString &rootPath, const QStringList &absolutePaths,
                          const std::atomic<bool> &cancel, int paceMs,
                          const std::function<void(int done, int total)> &progress);

} // namespace tw
