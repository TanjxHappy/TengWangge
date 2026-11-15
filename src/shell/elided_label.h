#pragma once

#include <QLabel>

namespace tw {

// 单行文字标签：过长时在末尾省略，完整内容放进工具提示。
// 最小宽度为 0，所以再长的文字也不会撑大窗口或状态栏。
class ElidedLabel : public QLabel
{
    Q_OBJECT

public:
    explicit ElidedLabel(QWidget *parent = nullptr);

    void setFullText(const QString &text);
    QString fullText() const { return m_fullText; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void updateElision();

    QString m_fullText;
};

} // namespace tw
