#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>

namespace tw {

// 用 QPainter 现画的线性图标，不带任何外部图片资源。ToolInfo/PageInfo 里的
// iconKey 在这里解析；未知或为空的 key 退回通用方块图标。
// 新增图标：在 icon_factory.cpp 的 drawGlyph() 里加一个 case，并写进 knownKeys()。
class IconFactory
{
public:
    static QStringList knownKeys();
    static QPixmap pixmap(const QString &key, int size, const QColor &color, qreal devicePixelRatio = 2.0);
    static QIcon icon(const QString &key, const QColor &normal, const QColor &selected,
                      const QColor &disabled);
};

} // namespace tw
