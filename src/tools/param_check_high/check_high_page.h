#pragma once

#include "core/platform_services.h"
#include "tools/param_check_high/notation.h"

#include <QWidget>

class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QSplitter;
class QTimer;
class QTreeView;

namespace tw {

class ElidedLabel;
class StructureModel;

// 原文、结构树、属性表单和问题列表四块对照。只有用户保存时才写文件。
class HighCheckPage : public QWidget
{
    Q_OBJECT

public:
    explicit HighCheckPage(const ToolServices &services, QWidget *parent = nullptr);
    ~HighCheckPage() override;

    void persistSession();

    bool openFile(const QString &path, QString *error = nullptr);
    bool saveFile(QString *error = nullptr);
    bool saveFileAs(const QString &path, QString *error = nullptr);

    void editSourceText(const QString &text);
    void queueSourceParse(const QString &text);
    QString sourceText() const;
    QString parseError() const { return m_error; }
    bool treeInSync() const { return m_treeInSync; }
    Notation activeFormat() const { return m_active; }
    bool hasNode(const QStringList &path) const;
    QString nodeText(const QStringList &path) const;
    bool setNodeText(const QStringList &path, const QString &value);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void applyIdle();
    void publish(const QString &text);
    void launchParse(const QString &text);
    void adopt(Sheet sheet);
    void onStructureEdited();
    void onOpen();
    void onSave();
    void onSaveAs();
    void onAdd();
    void onAddAttribute();
    void onRemove();
    void onTypeChanged(int index);
    void onExpandAll();
    void onCollapseAll();
    void findStep(bool backward);
    void refreshChrome();
    void fillProperty(const QModelIndex &current);
    void applyPropertyName();
    void applyPropertyValue();
    void refreshProblems();
    void focusSourceLine(int line, int column);
    void selectNodeForCursor();
    void openNodeDialog(const QModelIndex &index);
    QString pathText(const Node *node) const;
    void setBanner(const QString &text, bool error);
    void markLine(int line, int column);
    void updatePathLabel();
    bool confirmDiscard();
    Notation requested() const;
    static int countNodes(const Node *node);
    static Node *deepestCovering(Node *node, int line, int column, Node *best);

    ToolServices m_services;
    QString m_dataDir;
    QString m_path;
    QString m_suffix;
    QString m_lastDirectory;
    QString m_savedText;
    QString m_error;
    QString m_loggedError;
    QByteArray m_pendingSplitter;
    Notation m_active = Notation::Automatic;
    bool m_treeInSync = false;
    bool m_dirty = false;
    bool m_quiet = false;
    bool m_applying = false;
    bool m_notUtf8 = false;
    bool m_bom = false;
    bool m_crlf = false;
    bool m_splitApplied = false;
    bool m_busy = false;
    bool m_fromTree = false;
    bool m_formQuiet = false;
    int m_ticket = 0;
    int m_errorLine = 0;
    int m_errorColumn = 0;

    StructureModel *m_model = nullptr;
    QPlainTextEdit *m_source = nullptr;
    QTreeView *m_tree = nullptr;
    QSplitter *m_splitter = nullptr;
    QSplitter *m_stack = nullptr;
    QComboBox *m_format = nullptr;
    QComboBox *m_type = nullptr;
    QLineEdit *m_find = nullptr;
    QLineEdit *m_propName = nullptr;
    QLineEdit *m_propValue = nullptr;
    QLabel *m_detected = nullptr;
    QLabel *m_propKind = nullptr;
    QLabel *m_propLine = nullptr;
    QLabel *m_propChildren = nullptr;
    ElidedLabel *m_banner = nullptr;
    ElidedLabel *m_pathLabel = nullptr;
    ElidedLabel *m_propPath = nullptr;
    QListWidget *m_problems = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_saveAs = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_addAttribute = nullptr;
    QPushButton *m_remove = nullptr;
    QTimer *m_debounce = nullptr;
};

} // namespace tw
