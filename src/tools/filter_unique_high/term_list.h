#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace tw {

// 名单里的一条关键词。shown 保留用户第一次写下的样子，folded 用来比较。
struct Term
{
    QString shown;
    QString folded;
};

// 每行一个关键词。去掉首尾空白，忽略空行，按 Unicode 大小写折叠去重，保留第一次的写法。
QVector<Term> distillTerms(const QString &text);

// 子串匹配。相对路径或文件名任一命中即算，比较前做大小写折叠。同一条只记一次。
QStringList termsHitting(const QVector<Term> &terms, const QString &relativePath,
                         const QString &fileName);

} // namespace tw
