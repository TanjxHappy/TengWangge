#include "shell/elided_label.h"

#include <QEvent>
#include <QFontMetrics>

namespace tw {

ElidedLabel::ElidedLabel(QWidget *parent)
    : QLabel(parent)
{
    setTextFormat(Qt::PlainText);
    setWordWrap(false);
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
}

void ElidedLabel::setFullText(const QString &text)
{
    m_fullText = text;
    m_fullText.replace(QLatin1Char('\n'), QLatin1Char(' '));
    m_fullText.replace(QLatin1Char('\r'), QLatin1Char(' '));
    setToolTip(m_fullText);
    updateElision();
}

QSize ElidedLabel::sizeHint() const
{
    return QSize(0, fontMetrics().height() + 4);
}

QSize ElidedLabel::minimumSizeHint() const
{
    return QSize(0, fontMetrics().height() + 4);
}

void ElidedLabel::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    updateElision();
}

void ElidedLabel::showEvent(QShowEvent *event)
{
    QLabel::showEvent(event);
    updateElision();
}

void ElidedLabel::changeEvent(QEvent *event)
{
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        updateElision();
}

void ElidedLabel::updateElision()
{
    const int available = qMax(0, contentsRect().width());
    const QString elided = fontMetrics().elidedText(m_fullText, Qt::ElideRight, available);
    if (text() != elided)
        QLabel::setText(elided);
}

} // namespace tw
