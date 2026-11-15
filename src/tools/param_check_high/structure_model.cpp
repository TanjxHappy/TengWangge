#include "tools/param_check_high/structure_model.h"

namespace tw {
namespace {

bool sameSiblingName(const Node *node, const QString &name)
{
    if (node == nullptr || node->parent == nullptr)
        return false;
    for (int i = 0; i < node->parent->childCount(); ++i) {
        const Node *sibling = node->parent->children[static_cast<size_t>(i)].get();
        if (sibling == node)
            continue;
        if (sibling->name != name)
            continue;
        if (node->parent->kind == NodeKind::Map || sibling->kind == node->kind)
            return true;
    }
    return false;
}

} // namespace

StructureModel::StructureModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

void StructureModel::resetSheet(Sheet sheet, bool locked)
{
    beginResetModel();
    m_sheet = std::move(sheet);
    m_locked = locked;
    endResetModel();
}

void StructureModel::setLocked(bool locked)
{
    if (m_locked == locked)
        return;
    m_locked = locked;
    if (!m_sheet.root || rowCount() <= 0)
        return;
    const QModelIndex topLeft = index(0, 0);
    const QModelIndex bottomRight = index(rowCount() - 1, columnCount() - 1);
    emit dataChanged(topLeft, bottomRight);
}

bool StructureModel::assignPath(const QStringList &path, const QString &value, QString *error)
{
    if (m_locked || path.isEmpty() || !m_sheet.root) {
        if (error)
            *error = QStringLiteral("结构还不能编辑。请先修正左边的文本。");
        return false;
    }
    Node *node = findNode(m_sheet.root.get(), path);
    if (node == nullptr) {
        if (error)
            *error = QStringLiteral("没有找到这一项。");
        return false;
    }
    if (!assignValue(node, value, error)) {
        if (error != nullptr && !error->isEmpty())
            emit editRejected(*error);
        return false;
    }
    notifyValue(node);
    emit structureEdited();
    return true;
}

bool StructureModel::addChild(const QModelIndex &index)
{
    if (m_locked || !m_sheet.root)
        return false;
    Node *parentNode = addTarget(index);
    if (parentNode == nullptr)
        return false;
    std::unique_ptr<Node> child = makeChild(parentNode);
    if (!child)
        return false;
    const int row = parentNode->childCount();
    beginInsertRows(indexFor(parentNode), row, row);
    parentNode->append(std::move(child));
    endInsertRows();
    emit structureEdited();
    return true;
}

bool StructureModel::addAttribute(const QModelIndex &index)
{
    if (m_locked || m_sheet.notation != Notation::Xml)
        return false;
    Node *element = nodeFrom(index);
    if (element != nullptr && element->kind == NodeKind::Attribute)
        element = element->parent;
    if (element == nullptr || element->kind != NodeKind::Element)
        return false;

    int row = element->childCount();
    for (int i = 0; i < element->childCount(); ++i) {
        if (element->children[static_cast<size_t>(i)]->kind != NodeKind::Attribute) {
            row = i;
            break;
        }
    }
    auto attribute = std::make_unique<Node>();
    attribute->kind = NodeKind::Attribute;
    attribute->name = uniqueName(element, QStringLiteral("新属性"));
    beginInsertRows(indexFor(element), row, row);
    attribute->parent = element;
    element->children.insert(element->children.begin() + row, std::move(attribute));
    endInsertRows();
    emit structureEdited();
    return true;
}

bool StructureModel::removeNode(const QModelIndex &index)
{
    Node *node = nodeFrom(index);
    if (m_locked || node == nullptr || node->parent == nullptr || node == m_sheet.root.get())
        return false;
    if (isLastXmlRoot(node)) {
        emit editRejected(QStringLiteral("至少要留下一个根节点。"));
        return false;
    }
    Node *parentNode = node->parent;
    const int row = node->indexInParent();
    beginRemoveRows(indexFor(parentNode), row, row);
    parentNode->children.erase(parentNode->children.begin() + static_cast<std::ptrdiff_t>(row));
    endRemoveRows();
    emit structureEdited();
    return true;
}

bool StructureModel::setScalarKind(const QModelIndex &index, NodeKind kind, QString *error)
{
    Node *node = nodeFrom(index);
    if (m_locked || !isJsonScalar(node)) {
        if (error)
            *error = QStringLiteral("只有 JSON 的字符串、数字、布尔和空值可以改类型。");
        return false;
    }
    if (kind != NodeKind::Phrase && kind != NodeKind::Quantity && kind != NodeKind::Switch
        && kind != NodeKind::Vacant) {
        if (error)
            *error = QStringLiteral("不能改成这种类型。");
        return false;
    }
    if (node->kind != kind) {
        if (kind == NodeKind::Quantity) {
            const QString current = node->kind == NodeKind::Quantity ? displayedValue(node) : node->value;
            if (!isJsonNumber(current.trimmed())) {
                node->spelling = QStringLiteral("0");
                node->value = QStringLiteral("0");
            } else {
                node->spelling = current.trimmed();
                node->value = node->spelling;
            }
        } else if (kind == NodeKind::Switch) {
            node->value = QStringLiteral("true");
            node->spelling.clear();
        } else if (kind == NodeKind::Vacant) {
            node->value.clear();
            node->spelling.clear();
        } else {
            if (node->kind == NodeKind::Quantity)
                node->value = node->spelling.isEmpty() ? node->value : node->spelling;
            if (node->kind == NodeKind::Vacant)
                node->value.clear();
            node->spelling.clear();
        }
        node->kind = kind;
    }
    notifyValue(node);
    emit structureEdited();
    return true;
}

NodeKind StructureModel::scalarKind(const QModelIndex &index) const
{
    const Node *node = nodeFrom(index);
    if (!isJsonScalar(node))
        return NodeKind::Phrase;
    return node->kind;
}

bool StructureModel::canAdd(const QModelIndex &index) const
{
    return !m_locked && addTarget(index) != nullptr;
}

bool StructureModel::canAddAttribute(const QModelIndex &index) const
{
    if (m_locked || m_sheet.notation != Notation::Xml)
        return false;
    const Node *node = nodeFrom(index);
    if (node == nullptr)
        return false;
    if (node->kind == NodeKind::Element)
        return true;
    return node->kind == NodeKind::Attribute && node->parent != nullptr
           && node->parent->kind == NodeKind::Element;
}

bool StructureModel::canRemove(const QModelIndex &index) const
{
    const Node *node = nodeFrom(index);
    if (m_locked || node == nullptr || node->parent == nullptr || node == m_sheet.root.get())
        return false;
    return !isLastXmlRoot(node);
}

QVariant StructureModel::data(const QModelIndex &index, int role) const
{
    const Node *node = nodeFrom(index);
    if (node == nullptr)
        return {};
    if (role == Qt::ToolTipRole)
        return index.column() == 2 ? QVariant(displayedValue(node)) : QVariant(nodeLabel(node));
    if (role != Qt::DisplayRole && role != Qt::EditRole)
        return {};
    switch (index.column()) {
    case 0:
        if (role == Qt::EditRole && node->kind == NodeKind::Attribute)
            return node->name;
        return nodeLabel(node);
    case 1:
        return nodeKindCaption(node->kind);
    case 2:
        return displayedValue(node);
    default:
        return {};
    }
}

bool StructureModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (m_locked || role != Qt::EditRole)
        return false;
    Node *node = nodeFrom(index);
    if (node == nullptr)
        return false;
    QString error;
    bool ok = false;
    if (index.column() == 0)
        ok = renameNode(node, value.toString(), &error);
    else if (index.column() == 2)
        ok = assignValue(node, value.toString(), &error);
    if (!ok) {
        if (!error.isEmpty())
            emit editRejected(error);
        return false;
    }
    notifyValue(node);
    emit structureEdited();
    return true;
}

QVariant StructureModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case 0:
        return QStringLiteral("名称");
    case 1:
        return QStringLiteral("类型");
    case 2:
        return QStringLiteral("值");
    default:
        return {};
    }
}

QModelIndex StructureModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!m_sheet.root || column < 0 || column >= columnCount() || row < 0)
        return {};
    if (!parent.isValid()) {
        if (rootHidden()) {
            if (row >= m_sheet.root->childCount())
                return {};
            return createIndex(row, column, m_sheet.root->children[static_cast<size_t>(row)].get());
        }
        if (row != 0)
            return {};
        return createIndex(0, column, m_sheet.root.get());
    }
    Node *parentNode = nodeFrom(parent);
    if (parentNode == nullptr || row >= parentNode->childCount())
        return {};
    return createIndex(row, column, parentNode->children[static_cast<size_t>(row)].get());
}

QModelIndex StructureModel::parent(const QModelIndex &child) const
{
    Node *node = nodeFrom(child);
    if (node == nullptr || node->parent == nullptr)
        return {};
    if (node->parent == m_sheet.root.get() && rootHidden())
        return {};
    return indexFor(node->parent);
}

int StructureModel::rowCount(const QModelIndex &parent) const
{
    if (!m_sheet.root || parent.column() > 0)
        return 0;
    if (!parent.isValid())
        return rootHidden() ? m_sheet.root->childCount() : 1;
    const Node *node = nodeFrom(parent);
    return node == nullptr ? 0 : node->childCount();
}

int StructureModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return 3;
}

Qt::ItemFlags StructureModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags itemFlags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (m_locked)
        return itemFlags;
    const Node *node = nodeFrom(index);
    if (index.column() == 0 && nodeNameEditable(node))
        itemFlags |= Qt::ItemIsEditable;
    if (index.column() == 2 && nodeValueEditable(node))
        itemFlags |= Qt::ItemIsEditable;
    return itemFlags;
}

Node *StructureModel::nodeFrom(const QModelIndex &index) const
{
    if (!index.isValid())
        return nullptr;
    return static_cast<Node *>(index.internalPointer());
}

QModelIndex StructureModel::indexFor(Node *node) const
{
    if (node == nullptr || !m_sheet.root)
        return {};
    if (node == m_sheet.root.get())
        return rootHidden() ? QModelIndex() : createIndex(0, 0, node);
    return createIndex(node->indexInParent(), 0, node);
}

bool StructureModel::rootHidden() const
{
    if (!m_sheet.root)
        return false;
    const NodeKind kind = m_sheet.root->kind;
    return kind == NodeKind::Map || kind == NodeKind::List || kind == NodeKind::Document;
}

Node *StructureModel::addTarget(const QModelIndex &index) const
{
    Node *selected = nodeFrom(index);
    if (selected != nullptr && acceptsChildren(selected))
        return selected;
    if (selected != nullptr && selected->parent != nullptr && acceptsChildren(selected->parent))
        return selected->parent;
    if (m_sheet.root && acceptsChildren(m_sheet.root.get()))
        return m_sheet.root.get();
    return nullptr;
}

bool StructureModel::acceptsChildren(const Node *node) const
{
    if (node == nullptr)
        return false;
    switch (node->kind) {
    case NodeKind::Map:
    case NodeKind::List:
    case NodeKind::Element:
    case NodeKind::Section:
    case NodeKind::Document:
        return true;
    default:
        return false;
    }
}

QString StructureModel::uniqueName(const Node *parentNode, const QString &base) const
{
    auto taken = [parentNode](const QString &candidate) {
        for (int i = 0; i < parentNode->childCount(); ++i) {
            if (parentNode->children[static_cast<size_t>(i)]->name == candidate)
                return true;
        }
        return false;
    };
    if (!taken(base))
        return base;
    for (int number = 2; number < 10000; ++number) {
        const QString candidate = base + QString::number(number);
        if (!taken(candidate))
            return candidate;
    }
    return base + QStringLiteral("-x");
}

std::unique_ptr<Node> StructureModel::makeChild(Node *parentNode) const
{
    if (parentNode == nullptr)
        return nullptr;
    auto node = std::make_unique<Node>();
    switch (parentNode->kind) {
    case NodeKind::Map:
        node->kind = NodeKind::Phrase;
        node->name = uniqueName(parentNode, QStringLiteral("新键"));
        break;
    case NodeKind::List:
        node->kind = NodeKind::Phrase;
        break;
    case NodeKind::Element:
        node->kind = NodeKind::Element;
        node->name = uniqueName(parentNode, QStringLiteral("新节点"));
        {
            auto text = std::make_unique<Node>();
            text->kind = NodeKind::Characters;
            node->append(std::move(text));
        }
        break;
    case NodeKind::Section:
        node->kind = NodeKind::Field;
        node->name = uniqueName(parentNode, QStringLiteral("新键"));
        break;
    case NodeKind::Document:
        if (m_sheet.notation == Notation::Xml) {
            node->kind = NodeKind::Element;
            node->name = uniqueName(parentNode, QStringLiteral("新节点"));
        } else {
            node->kind = NodeKind::Section;
            node->name = uniqueName(parentNode, QStringLiteral("新节"));
        }
        break;
    default:
        return nullptr;
    }
    return node;
}

bool StructureModel::assignValue(Node *node, const QString &text, QString *error)
{
    if (node == nullptr)
        return false;
    if (node->kind == NodeKind::Quantity) {
        const QString spelling = text.trimmed();
        if (!isJsonNumber(spelling)) {
            if (error)
                *error = QStringLiteral("数字格式不对。请写成 1、1.50 或 -2 这样，不要加引号。");
            return false;
        }
        node->spelling = spelling;
        node->value = spelling;
        return true;
    }
    if (node->kind == NodeKind::Switch) {
        const QString token = text.trimmed().toLower();
        if (token == QLatin1String("true") || token == QLatin1String("是") || token == QLatin1String("真")) {
            node->value = QStringLiteral("true");
            return true;
        }
        if (token == QLatin1String("false") || token == QLatin1String("否") || token == QLatin1String("假")) {
            node->value = QStringLiteral("false");
            return true;
        }
        if (error)
            *error = QStringLiteral("布尔值只能填 true 或 false。");
        return false;
    }
    if (node->kind == NodeKind::Phrase || node->kind == NodeKind::Field || node->kind == NodeKind::Attribute
        || node->kind == NodeKind::Characters || node->kind == NodeKind::Comment) {
        node->value = text;
        return true;
    }
    if (node->kind == NodeKind::Element) {
        for (int i = 0; i < node->childCount(); ++i) {
            if (node->children[static_cast<size_t>(i)]->kind == NodeKind::Element) {
                if (error)
                    *error = QStringLiteral("这一项里面还有子节点，请直接改子节点。");
                return false;
            }
        }
        Node *textNode = nullptr;
        for (int i = 0; i < node->childCount(); ++i) {
            if (node->children[static_cast<size_t>(i)]->kind == NodeKind::Characters) {
                textNode = node->children[static_cast<size_t>(i)].get();
                break;
            }
        }
        if (textNode != nullptr) {
            textNode->value = text;
            notifyValue(textNode);
            return true;
        }
        auto created = std::make_unique<Node>();
        created->kind = NodeKind::Characters;
        created->value = text;
        const int row = node->childCount();
        beginInsertRows(indexFor(node), row, row);
        node->append(std::move(created));
        endInsertRows();
        return true;
    }
    if (error)
        *error = QStringLiteral("这一项是一组结构，请改里面的具体值。");
    return false;
}

bool StructureModel::renameNode(Node *node, const QString &text, QString *error)
{
    if (!nodeNameEditable(node)) {
        if (error)
            *error = QStringLiteral("这一项的名字不能修改。");
        return false;
    }
    QString cleaned = text.trimmed();
    if (node->kind == NodeKind::Attribute && cleaned.startsWith(QLatin1Char('@')))
        cleaned.remove(0, 1);
    if (cleaned.isEmpty()) {
        if (error)
            *error = QStringLiteral("名字不能是空的。");
        return false;
    }
    if (cleaned.contains(QLatin1Char('\n')) || cleaned.contains(QLatin1Char('\r'))) {
        if (error)
            *error = QStringLiteral("名字里不能换行。");
        return false;
    }
    if (node->kind == NodeKind::Section
        && (cleaned.contains(QLatin1Char('[')) || cleaned.contains(QLatin1Char(']')))) {
        if (error)
            *error = QStringLiteral("小节名字里不能有方括号。");
        return false;
    }
    if (node->kind == NodeKind::Field && cleaned.contains(QLatin1Char('='))) {
        if (error)
            *error = QStringLiteral("键名里不能有等号。");
        return false;
    }
    if (sameSiblingName(node, cleaned)) {
        if (error)
            *error = QStringLiteral("同一个位置已经有「%1」。请换一个名字。").arg(cleaned.left(40));
        return false;
    }
    node->name = cleaned;
    return true;
}

void StructureModel::notifyValue(Node *node)
{
    const QModelIndex rowIndex = indexFor(node);
    if (!rowIndex.isValid())
        return;
    emit dataChanged(rowIndex.sibling(rowIndex.row(), 0), rowIndex.sibling(rowIndex.row(), columnCount() - 1),
                     {Qt::DisplayRole, Qt::EditRole});
}

bool StructureModel::isLastXmlRoot(const Node *node) const
{
    if (m_sheet.notation != Notation::Xml || node == nullptr || node->kind != NodeKind::Element)
        return false;
    if (node->parent == nullptr || node->parent->kind != NodeKind::Document)
        return false;
    int elements = 0;
    for (int i = 0; i < node->parent->childCount(); ++i) {
        if (node->parent->children[static_cast<size_t>(i)]->kind == NodeKind::Element)
            ++elements;
    }
    return elements <= 1;
}

} // namespace tw
