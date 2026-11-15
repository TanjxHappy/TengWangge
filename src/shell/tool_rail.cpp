#include "shell/tool_rail.h"

#include "shell/icon_factory.h"
#include "shell/theme.h"

#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QStyledItemDelegate>

namespace tw {
namespace {

constexpr int kRowHeight = 42;
constexpr int kAccentWidth = 3;
constexpr int kIconSize = 18;

class ToolRailDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return QSize(option.rect.width(), kRowHeight);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const Theme &theme = Theme::workbench();
        const bool enabled = option.state & QStyle::State_Enabled;
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = (option.state & QStyle::State_MouseOver) && enabled;
        const bool focused = (option.state & QStyle::State_HasFocus) && enabled;
        const bool available = index.data(ToolRail::AvailableRole).toBool();
        const QRect rect = option.rect;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);

        if (selected)
            painter->fillRect(rect, theme.chromeSelected);
        else if (hovered)
            painter->fillRect(rect, theme.chromeHover);

        if (selected) {
            painter->fillRect(QRect(rect.left(), rect.top(), kAccentWidth, rect.height()),
                              enabled ? theme.accent : theme.chromeDisabled);
        }

        QColor textColor = theme.chromeText;
        if (!enabled)
            textColor = selected ? theme.chromeMuted : theme.chromeDisabled;
        else if (!available)
            textColor = theme.chromeDisabled;
        else if (!selected)
            textColor = theme.chromeMuted.lighter(125);

        const QString iconKey = index.data(ToolRail::IconKeyRole).toString();
        const QColor iconColor = (selected && enabled && available) ? theme.accent : textColor;
        const QPixmap icon = IconFactory::pixmap(iconKey, kIconSize, iconColor,
                                                 painter->device()->devicePixelRatioF());
        const int iconY = rect.top() + (rect.height() - kIconSize) / 2;
        painter->drawPixmap(rect.left() + 14, iconY, icon);

        QFont font = option.font;
        font.setBold(selected);
        painter->setFont(font);
        painter->setPen(textColor);

        const QString shortcut = index.data(ToolRail::ShortcutRole).toString();
        const QFontMetrics metrics(font);
        int rightEdge = rect.right() - 10;

        QFont smallFont = option.font;
        smallFont.setPixelSize(qMax(10, option.font.pixelSize() - 2));
        smallFont.setBold(false);
        const QFontMetrics smallMetrics(smallFont);

        if (!available) {
            const QString badge = QStringLiteral("不可用");
            const QRect badgeRect(rightEdge - smallMetrics.horizontalAdvance(badge) - 10,
                                  rect.top() + (rect.height() - 16) / 2,
                                  smallMetrics.horizontalAdvance(badge) + 10, 16);
            painter->setFont(smallFont);
            painter->setPen(QPen(theme.chromeDisabled, 1));
            painter->drawRoundedRect(badgeRect, 2, 2);
            painter->drawText(badgeRect, Qt::AlignCenter, badge);
            rightEdge = badgeRect.left() - 6;
            painter->setFont(font);
            painter->setPen(textColor);
        } else if (!shortcut.isEmpty()) {
            painter->setFont(smallFont);
            painter->setPen(enabled ? theme.chromeDisabled.lighter(130) : theme.chromeDisabled);
            const QString hint = QStringLiteral("Ctrl+") + shortcut;
            const int hintWidth = smallMetrics.horizontalAdvance(hint);
            painter->drawText(QRect(rightEdge - hintWidth, rect.top(), hintWidth, rect.height()),
                              Qt::AlignVCenter | Qt::AlignRight, hint);
            rightEdge -= hintWidth + 8;
            painter->setFont(font);
            painter->setPen(textColor);
        }

        const int textLeft = rect.left() + 14 + kIconSize + 10;
        const QRect textRect(textLeft, rect.top(), qMax(0, rightEdge - textLeft), rect.height());
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                          metrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                                             textRect.width()));

        if (focused) {
            painter->setPen(QPen(theme.accent, 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(rect.adjusted(1, 1, -2, -2));
        }
        painter->restore();
    }
};

} // namespace

ToolRail::ToolRail(QWidget *parent)
    : QListWidget(parent)
{
    setObjectName(QStringLiteral("toolRail"));
    setAccessibleName(QStringLiteral("工具列表"));
    setItemDelegate(new ToolRailDelegate(this));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setUniformItemSizes(true);
    setToolTip(QString());
}

void ToolRail::setTools(const QVector<ToolInfo> &tools)
{
    clear();
    for (int i = 0; i < tools.size(); ++i) {
        const ToolInfo &info = tools.at(i);
        auto *item = new QListWidgetItem(info.title);
        item->setData(IconKeyRole, info.iconKey);
        item->setData(AvailableRole, true);
        if (i < 9)
            item->setData(ShortcutRole, QString::number(i + 1));
        item->setToolTip(info.summary.isEmpty() ? info.title : info.summary);
        item->setData(Qt::AccessibleTextRole, info.title);
        addItem(item);
    }
}

void ToolRail::setAvailable(int index, bool available, const QString &reason)
{
    QListWidgetItem *entry = item(index);
    if (!entry)
        return;
    entry->setData(AvailableRole, available);
    if (!available) {
        entry->setToolTip(reason.isEmpty() ? QStringLiteral("工具不可用") : reason);
    }
    viewport()->update();
}

bool ToolRail::isAvailable(int index) const
{
    const QListWidgetItem *entry = item(index);
    return entry && entry->data(AvailableRole).toBool();
}

QSize ToolRail::sizeHint() const
{
    return QSize(184, kRowHeight * qMax(1, count()));
}

} // namespace tw
