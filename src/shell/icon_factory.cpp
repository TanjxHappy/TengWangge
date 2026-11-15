#include "shell/icon_factory.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>

namespace tw {
namespace {

// 在 16x16 的逻辑坐标里绘制。
void drawGlyph(QPainter &p, const QString &key, const QColor &color)
{
    if (key == QLatin1String("overview")) {
        for (qreal x : {2.0, 9.0})
            for (qreal y : {2.0, 9.0})
                p.drawRoundedRect(QRectF(x, y, 5.0, 5.0), 1.0, 1.0);
    } else if (key == QLatin1String("task")) {
        p.drawEllipse(QRectF(1.5, 1.5, 13.0, 13.0));
        p.setBrush(color);
        p.drawPolygon(QPolygonF({QPointF(6.4, 5.0), QPointF(11.4, 8.0), QPointF(6.4, 11.0)}));
    } else if (key == QLatin1String("status")) {
        QPolygonF pulse;
        pulse << QPointF(1.5, 8.5) << QPointF(5.0, 8.5) << QPointF(7.0, 3.0)
              << QPointF(9.5, 13.0) << QPointF(11.0, 8.5) << QPointF(14.5, 8.5);
        p.drawPolyline(pulse);
    } else if (key == QLatin1String("info")) {
        p.drawEllipse(QRectF(1.5, 1.5, 13.0, 13.0));
        p.drawLine(QPointF(8.0, 7.2), QPointF(8.0, 11.5));
        p.drawPoint(QPointF(8.0, 4.8));
    } else if (key == QLatin1String("environment")) {
        p.drawRoundedRect(QRectF(1.5, 2.5, 13.0, 9.0), 1.2, 1.2);
        p.drawLine(QPointF(5.5, 14.0), QPointF(10.5, 14.0));
        p.drawLine(QPointF(8.0, 11.5), QPointF(8.0, 14.0));
    } else if (key == QLatin1String("log")) {
        for (qreal y : {3.5, 8.0, 12.5})
            p.drawLine(QPointF(2.5, y), QPointF(13.5, y));
    } else if (key == QLatin1String("lock")) {
        p.drawRoundedRect(QRectF(3.0, 7.0, 10.0, 7.0), 1.2, 1.2);
        QPainterPath shackle;
        shackle.moveTo(5.2, 7.0);
        shackle.lineTo(5.2, 5.0);
        shackle.arcTo(QRectF(5.2, 2.0, 5.6, 6.0), 180, -180);
        shackle.lineTo(10.8, 7.0);
        p.drawPath(shackle);
    } else {
        p.drawRoundedRect(QRectF(2.5, 2.5, 11.0, 11.0), 2.0, 2.0);
    }
}

} // namespace

QStringList IconFactory::knownKeys()
{
    return {QStringLiteral("overview"), QStringLiteral("task"),   QStringLiteral("status"),
            QStringLiteral("info"),     QStringLiteral("environment"), QStringLiteral("log"),
            QStringLiteral("lock")};
}

QPixmap IconFactory::pixmap(const QString &key, int size, const QColor &color, qreal devicePixelRatio)
{
    const int pixels = qMax(1, qRound(size * devicePixelRatio));
    QPixmap pixmap(pixels, pixels);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(devicePixelRatio);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(size / 16.0, size / 16.0);
    QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    drawGlyph(painter, key, color);
    return pixmap;
}

QIcon IconFactory::icon(const QString &key, const QColor &normal, const QColor &selected,
                        const QColor &disabled)
{
    QIcon icon;
    icon.addPixmap(pixmap(key, 16, normal), QIcon::Normal, QIcon::Off);
    icon.addPixmap(pixmap(key, 16, selected), QIcon::Selected, QIcon::Off);
    icon.addPixmap(pixmap(key, 16, selected), QIcon::Active, QIcon::Off);
    icon.addPixmap(pixmap(key, 16, disabled), QIcon::Disabled, QIcon::Off);
    return icon;
}

} // namespace tw
