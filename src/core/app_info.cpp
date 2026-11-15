#include "core/app_info.h"

#ifndef TENGWANGGE_VERSION
#define TENGWANGGE_VERSION "0.0.0"
#endif

namespace tw {

QString AppInfo::productName()
{
    return QStringLiteral("滕王阁");
}

QString AppInfo::internalName()
{
    return QStringLiteral("TengWangge");
}

QString AppInfo::version()
{
    return QStringLiteral(TENGWANGGE_VERSION);
}

QString AppInfo::dataDirName()
{
    return internalName();
}

} // namespace tw
