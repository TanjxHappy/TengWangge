#pragma once

#include "core/platform_services.h"
#include "tools/param_check/param_document.h"

#include <QWidget>

#include <memory>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QSplitter;
class QTimer;
class QTreeView;

namespace tw {

class ElidedLabel;
class ParamTreeModel;
struct ParamAsyncParse;

// 左右两栏：左侧是源文本，右侧是可编辑的结构树。只有用户保存时才写文件。
class ParamEditorPage : public QWidget
{
    Q_OBJECT

public:
    explicit ParamEditorPage(const ToolServices &services, QWidget *parent = nullptr);
    ~ParamEditorPage() override;

    void persistSession();

    bool openFile(const QString &path, QString *error = nullptr);
    bool saveFile(QString *error = nullptr);
    bool saveFileAs(const QString &path, QString *error = nullptr);

    void editSourceText(const QString &text);
    void flushParse();
    QString sourceText() const;
    QString parseError() const { return m_errorText; }
    bool treeInSync() const { return m_treeInSync; }
    ParamFormat activeFormat() const { return m_activeFormat; }
    bool hasNode(const QStringList &path) const;
    QString nodeText(const QStringList &path) const;
    bool setNodeText(const QStringList &path, const QString &value);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void buildUi();
    void beginParse(bool forceSync, bool dropTreeOnFailure);
    void acceptDocument(ParamDocument document, bool dropTreeOnFailure, bool fromAsync);
    void onAsyncFinished();
    void clearTree();
    bool expandParsedTree();
    void onTextChanged();
    void onDocumentEdited();
    void onOpen();
    void onSave();
    void onAdd();
    void onAddAttribute();
    void onRemove();
    void onTypeChanged(int index);
    void updateChrome();
    enum class Notice { Clear, Info, Busy, Error };
    void setNotice(const QString &text, Notice notice);
    void recomputeDirty();
    bool textIsWellFormedUtf16(const QString &text) const;
    bool confirmDiscard();
    bool saveAsInteractive(QString *error);
    ParamFormat requestedFormat() const;

    ToolServices m_services;
    QString m_dataDir;
    QString m_path;
    QString m_suffix;
    QString m_lastDirectory;
    QString m_savedText;
    QString m_errorText;
    QByteArray m_pendingSplitter;
    ParamFormat m_activeFormat = ParamFormat::Unsupported;
    bool m_treeInSync = false;
    bool m_dirty = false;
    bool m_applying = false;
    bool m_preferSuffix = false;
    bool m_lossyUnedited = false;
    bool m_utf8Bom = false;
    bool m_crlf = false;
    bool m_parsedOnce = false;
    bool m_splitterRestored = false;
    bool m_asyncBusy = false;
    bool m_asyncRestart = false;
    bool m_restartDropTree = false;
    quint64 m_parseGeneration = 0;
    quint64 m_inflightGeneration = 0;
    QString m_saveBlockedReason;
    std::shared_ptr<ParamAsyncParse> m_inflight;

    ParamTreeModel *m_model = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    QTreeView *m_tree = nullptr;
    QSplitter *m_splitter = nullptr;
    QComboBox *m_format = nullptr;
    QComboBox *m_type = nullptr;
    QLabel *m_detected = nullptr;
    QLabel *m_banner = nullptr;
    ElidedLabel *m_pathLabel = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_addAttribute = nullptr;
    QPushButton *m_remove = nullptr;
    QTimer *m_parseTimer = nullptr;
};

} // namespace tw
