#include "shell/log_panel.h"

#include "shell/theme.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>

namespace tw {
namespace {

QColor levelColor(LogLevel level)
{
    switch (level) {
    case LogLevel::Success: return QColor(0x1e, 0x7a, 0x3c);
    case LogLevel::Warning: return QColor(0x9a, 0x67, 0x00);
    case LogLevel::Error: return Theme::workbench().danger;
    case LogLevel::Info: break;
    }
    return Theme::workbench().text;
}

} // namespace

LogPanel::LogPanel(LogBuffer *buffer, QWidget *parent)
    : QFrame(parent)
    , m_buffer(buffer)
{
    setObjectName(QStringLiteral("logPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAccessibleName(QStringLiteral("日志面板"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new QFrame;
    header->setObjectName(QStringLiteral("logHeader"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 4, 8, 4);
    headerLayout->setSpacing(10);

    m_title = new QLabel;
    m_title->setObjectName(QStringLiteral("logTitle"));
    m_showAll = new QCheckBox(QStringLiteral("显示全部工具"));
    m_showAll->setObjectName(QStringLiteral("logShowAll"));
    m_clear = new QPushButton(QStringLiteral("清空"));
    m_clear->setObjectName(QStringLiteral("logClear"));
    m_clear->setToolTip(QStringLiteral("清空平台日志缓冲（所有工具的日志）"));
    m_close = new QPushButton(QStringLiteral("收起"));
    m_close->setObjectName(QStringLiteral("logClose"));
    m_close->setToolTip(QStringLiteral("收起日志面板 (Ctrl+L)"));

    headerLayout->addWidget(m_title);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_showAll);
    headerLayout->addWidget(m_clear);
    headerLayout->addWidget(m_close);
    layout->addWidget(header);

    m_view = new QPlainTextEdit;
    m_view->setObjectName(QStringLiteral("logView"));
    m_view->setReadOnly(true);
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_view->setMaximumBlockCount(2000);
    m_view->setAccessibleName(QStringLiteral("日志内容"));
    layout->addWidget(m_view, 1);

    connect(m_showAll, &QCheckBox::toggled, this, [this] {
        updateTitle();
        reload();
    });
    connect(m_clear, &QPushButton::clicked, this, [this] { m_buffer->clear(); });
    connect(m_close, &QPushButton::clicked, this, &LogPanel::closeRequested);
    connect(m_buffer, &LogBuffer::entryAppended, this, [this](const LogEntry &entry) {
        if (matches(entry))
            appendEntry(entry);
    });
    connect(m_buffer, &LogBuffer::cleared, this, [this] { m_view->clear(); });

    updateTitle();
    reload();
}

void LogPanel::setCurrentTool(const QString &toolId, const QString &toolTitle)
{
    if (m_toolId == toolId && m_toolTitle == toolTitle)
        return;
    m_toolId = toolId;
    m_toolTitle = toolTitle;
    updateTitle();
    if (!showsAllTools())
        reload();
}

void LogPanel::setTitleResolver(TitleResolver resolver)
{
    m_resolver = std::move(resolver);
    if (showsAllTools())
        reload();
}

bool LogPanel::showsAllTools() const
{
    return m_showAll->isChecked();
}

void LogPanel::setShowsAllTools(bool all)
{
    m_showAll->setChecked(all);
}

QString LogPanel::plainText() const
{
    return m_view->toPlainText();
}

bool LogPanel::matches(const LogEntry &entry) const
{
    if (showsAllTools() || m_toolId.isEmpty())
        return true;
    return entry.toolId.isEmpty() || entry.toolId == m_toolId;
}

void LogPanel::updateTitle()
{
    if (showsAllTools() || m_toolTitle.isEmpty())
        m_title->setText(QStringLiteral("日志 · 全部工具"));
    else
        m_title->setText(QStringLiteral("日志 · %1").arg(m_toolTitle));
}

void LogPanel::reload()
{
    m_view->clear();
    const QVector<LogEntry> all = m_buffer->entries();
    for (const LogEntry &entry : all) {
        if (matches(entry))
            appendEntry(entry);
    }
}

void LogPanel::appendEntry(const LogEntry &entry)
{
    QScrollBar *bar = m_view->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum() - 2;

    QString line = LogBuffer::format(entry);
    if (showsAllTools()) {
        QString source = QStringLiteral("平台");
        if (!entry.toolId.isEmpty())
            source = m_resolver ? m_resolver(entry.toolId) : entry.toolId;
        if (source.isEmpty())
            source = entry.toolId;
        line = QStringLiteral("%1  <%2>  [%3]  %4")
                   .arg(entry.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), source,
                        LogBuffer::levelName(entry.level), entry.message);
    }

    QTextCursor cursor(m_view->document());
    cursor.movePosition(QTextCursor::End);
    QTextCharFormat format;
    format.setForeground(levelColor(entry.level));
    if (!m_view->document()->isEmpty())
        cursor.insertBlock();
    cursor.insertText(line, format);

    if (atEnd)
        bar->setValue(bar->maximum());
}

} // namespace tw
