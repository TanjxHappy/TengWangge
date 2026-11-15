#pragma once

#include "tools/param_check_high/notation.h"

#include <QAbstractItemModel>

namespace tw {

class StructureModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    explicit StructureModel(QObject *parent = nullptr);

    void resetSheet(Sheet sheet, bool locked);
    void setLocked(bool locked);
    bool isLocked() const { return m_locked; }
    const Sheet &sheet() const { return m_sheet; }

    bool assignPath(const QStringList &path, const QString &value, QString *error);
    bool addChild(const QModelIndex &index);
    bool addAttribute(const QModelIndex &index);
    bool removeNode(const QModelIndex &index);
    bool setScalarKind(const QModelIndex &index, NodeKind kind, QString *error);
    NodeKind scalarKind(const QModelIndex &index) const;
    bool canAdd(const QModelIndex &index) const;
    bool canAddAttribute(const QModelIndex &index) const;
    bool canRemove(const QModelIndex &index) const;

    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

signals:
    void structureEdited();
    void editRejected(const QString &reason);

private:
    Node *nodeFrom(const QModelIndex &index) const;
    QModelIndex indexFor(Node *node) const;
    bool rootHidden() const;
    Node *addTarget(const QModelIndex &index) const;
    bool acceptsChildren(const Node *node) const;
    QString uniqueName(const Node *parentNode, const QString &base) const;
    std::unique_ptr<Node> makeChild(Node *parentNode) const;
    bool assignValue(Node *node, const QString &text, QString *error);
    bool renameNode(Node *node, const QString &text, QString *error);
    void notifyValue(Node *node);
    bool isLastXmlRoot(const Node *node) const;

    Sheet m_sheet;
    bool m_locked = true;
};

} // namespace tw
