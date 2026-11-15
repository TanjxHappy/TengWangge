#include "core/tool_info.h"

#include <QRegularExpression>

namespace tw {

bool isValidIdentifier(const QString &id)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$"));
    return id.size() <= 48 && pattern.match(id).hasMatch();
}

} // namespace tw
