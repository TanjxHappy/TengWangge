#pragma once

#include "core/log_buffer.h"

#include <QFrame>
#include <functional>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace tw {

// 底部日志面板，读平台唯一的 LogBuffer。
// 默认只显示“当前工具 + 平台自身”的条目，勾选“显示全部工具”后显示所有来源并带来源前缀。
// 面板只负责显示，不拥有日志。
class LogPanel : public QFrame
{
    Q_OBJECT

public:
    using TitleResolver = std::function<QString(const QString &toolId)>;

    explicit LogPanel(LogBuffer *buffer, QWidget *parent = nullptr);

    void setCurrentTool(const QString &toolId, const QString &toolTitle);
    void setTitleResolver(TitleResolver resolver);
    bool showsAllTools() const;
    void setShowsAllTools(bool all);

    QPlainTextEdit *view() const { return m_view; }
    QCheckBox *showAllCheck() const { return m_showAll; }
    QString plainText() const;

signals:
    void closeRequested();

private:
    void reload();
    void appendEntry(const LogEntry &entry);
    bool matches(const LogEntry &entry) const;
    void updateTitle();

    LogBuffer *m_buffer;
    QString m_toolId;
    QString m_toolTitle;
    TitleResolver m_resolver;
    QLabel *m_title = nullptr;
    QCheckBox *m_showAll = nullptr;
    QPushButton *m_clear = nullptr;
    QPushButton *m_close = nullptr;
    QPlainTextEdit *m_view = nullptr;
};

} // namespace tw
