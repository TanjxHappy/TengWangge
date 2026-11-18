#include "tools/filter_unique_high/term_list.h"

#include <QSet>

namespace tw {

QVector<Term> distillTerms(const QString &text)
{
    QVector<Term> terms;
    QSet<QString> seen;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        const QString folded = line.toCaseFolded();
        if (folded.isEmpty() || seen.contains(folded))
            continue;
        seen.insert(folded);
        terms.append(Term{line, folded});
    }
    return terms;
}

QStringList termsHitting(const QVector<Term> &terms, const QString &relativePath,
                         const QString &fileName)
{
    const QString foldedPath = relativePath.toCaseFolded();
    const QString foldedName = fileName.toCaseFolded();
    QStringList hits;
    for (const Term &term : terms) {
        if (term.folded.isEmpty())
            continue;
        if (foldedPath.contains(term.folded) || foldedName.contains(term.folded))
            hits.append(term.shown);
    }
    return hits;
}

} // namespace tw
