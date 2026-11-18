#pragma once

#include "tools/param_check/param_document.h"

#include <QAbstractItemModel>
#include <QStringList>

namespace tw {

class ParamTreeModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    explicit ParamTreeModel(QObject *parent = nullptr);

    void setDocument(ParamDocument document);
    const ParamDocument &document() const { return m_doc; }
    void setLocked(bool locked);
    bool isLocked() const { return m_locked; }

    ParamNode *nodeAt(const QModelIndex &index) const;
    QModelIndex indexForPath(const QStringList &path) const;
    bool canRemove(const QModelIndex &index) const;
    bool canAddAttribute(const QModelIndex &index) const;

    bool addChild(const QModelIndex &index);
    bool addAttribute(const QModelIndex &index);
    bool removeNode(const QModelIndex &index);
    bool setScalarKind(const QModelIndex &index, ParamKind kind);

    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

signals:
    void documentEdited();
    void editRejected(const QString &reason);

private:
    ParamNode *parentNode(const QModelIndex &parent) const;
    QModelIndex indexOf(ParamNode *node, int column) const;
    bool containsChildren(const ParamNode *node) const;
    std::unique_ptr<ParamNode> makeInserted(ParamNode *parentNode) const;
    void renumberArray(ParamNode *array);
    bool isLastXmlRoot(const ParamNode *node) const;
    QString uniqueName(const ParamNode *parentNode, const QString &base) const;

    ParamDocument m_doc;
    bool m_locked = true;
};

} // namespace tw
