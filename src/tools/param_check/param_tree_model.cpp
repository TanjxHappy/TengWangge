#include "tools/param_check/param_tree_model.h"

namespace tw {
namespace {

bool isJsonScalar(ParamKind kind)
{
    return kind == ParamKind::String || kind == ParamKind::Number || kind == ParamKind::Bool
        || kind == ParamKind::Null;
}

bool xmlNameOk(const QString &name)
{
    if (name.isEmpty())
        return false;
    const QChar first = name.at(0);
    if (!first.isLetter() && first != QLatin1Char('_') && first != QLatin1Char(':'))
        return false;
    for (const QChar c : name) {
        if (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('.')
            || c == QLatin1Char(':'))
            continue;
        return false;
    }
    return true;
}

ParamNode *findIn(ParamNode *node, const QString &part)
{
    if (!node)
        return nullptr;
    const bool wantAttribute = part.startsWith(QLatin1Char('@'));
    const QString key = wantAttribute ? part.mid(1) : part;
    for (const auto &child : node->children) {
        if (wantAttribute) {
            if (child->kind == ParamKind::Attribute && child->name == key)
                return child.get();
            continue;
        }
        if (child->name == part || paramNodeTitle(child.get()) == part)
            return child.get();
    }
    return nullptr;
}

} // namespace

ParamTreeModel::ParamTreeModel(QObject *parent)
    : QAbstractItemModel(parent)
{
    m_doc.root = std::make_unique<ParamNode>();
    m_doc.root->kind = ParamKind::Document;
    m_doc.format = ParamFormat::Unsupported;
}

void ParamTreeModel::setDocument(ParamDocument document)
{
    beginResetModel();
    m_doc = std::move(document);
    if (!m_doc.root) {
        m_doc.root = std::make_unique<ParamNode>();
        m_doc.root->kind = ParamKind::Document;
    }
    endResetModel();
}

void ParamTreeModel::setLocked(bool locked)
{
    if (m_locked == locked)
        return;
    m_locked = locked;
    emit layoutChanged();
}

ParamNode *ParamTreeModel::nodeAt(const QModelIndex &index) const
{
    if (!index.isValid())
        return nullptr;
    return static_cast<ParamNode *>(index.internalPointer());
}

ParamNode *ParamTreeModel::parentNode(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return m_doc.root.get();
    return nodeAt(parent);
}

QModelIndex ParamTreeModel::indexOf(ParamNode *node, int column) const
{
    if (!node || !node->parent)
        return {};
    return createIndex(node->rowInParent(), column, node);
}

QModelIndex ParamTreeModel::indexForPath(const QStringList &path) const
{
    ParamNode *node = m_doc.root.get();
    for (const QString &part : path) {
        node = findIn(node, part);
        if (!node)
            return {};
    }
    if (!node || node == m_doc.root.get())
        return {};
    return indexOf(node, 0);
}

bool ParamTreeModel::containsChildren(const ParamNode *node) const
{
    if (!node)
        return false;
    switch (node->kind) {
    case ParamKind::Object:
    case ParamKind::Array:
    case ParamKind::Section:
    case ParamKind::Element:
        return true;
    case ParamKind::Document:
        return m_doc.format != ParamFormat::Json;
    default:
        return false;
    }
}

QString ParamTreeModel::uniqueName(const ParamNode *parentNode, const QString &base) const
{
    QStringList used;
    for (const auto &child : parentNode->children)
        used.append(child->name);
    if (!used.contains(base))
        return base;
    for (int i = 2; i < 1000; ++i) {
        const QString candidate = base + QString::number(i);
        if (!used.contains(candidate))
            return candidate;
    }
    return base + QStringLiteral("-new");
}

std::unique_ptr<ParamNode> ParamTreeModel::makeInserted(ParamNode *parentNode) const
{
    if (!parentNode)
        return nullptr;
    auto node = std::make_unique<ParamNode>();
    switch (parentNode->kind) {
    case ParamKind::Array:
        node->kind = ParamKind::String;
        node->name = QStringLiteral("[%1]").arg(parentNode->childCount());
        break;
    case ParamKind::Element:
        node->kind = ParamKind::Element;
        node->name = uniqueName(parentNode, QStringLiteral("新元素"));
        break;
    case ParamKind::Section:
        node->kind = ParamKind::Key;
        node->name = uniqueName(parentNode, QStringLiteral("新键"));
        break;
    case ParamKind::Object:
        node->kind = ParamKind::String;
        node->name = uniqueName(parentNode, QStringLiteral("新键"));
        break;
    case ParamKind::Document:
        if (m_doc.format == ParamFormat::Xml) {
            node->kind = ParamKind::Element;
            node->name = uniqueName(parentNode, QStringLiteral("新元素"));
        } else if (m_doc.format == ParamFormat::Ini) {
            node->kind = ParamKind::Section;
            node->name = uniqueName(parentNode, QStringLiteral("新节"));
        } else {
            return nullptr;
        }
        break;
    default:
        return nullptr;
    }
    return node;
}

void ParamTreeModel::renumberArray(ParamNode *array)
{
    if (!array || array->kind != ParamKind::Array)
        return;
    for (int i = 0; i < array->childCount(); ++i)
        array->children[static_cast<size_t>(i)]->name = QStringLiteral("[%1]").arg(i);
}

bool ParamTreeModel::isLastXmlRoot(const ParamNode *node) const
{
    if (m_doc.format != ParamFormat::Xml || !node || node->kind != ParamKind::Element)
        return false;
    if (!node->parent || node->parent != m_doc.root.get())
        return false;
    int elements = 0;
    for (const auto &child : m_doc.root->children) {
        if (child->kind == ParamKind::Element)
            ++elements;
    }
    return elements <= 1;
}

bool ParamTreeModel::canRemove(const QModelIndex &index) const
{
    const ParamNode *node = nodeAt(index);
    if (m_locked || !node || !node->parent)
        return false;
    return !isLastXmlRoot(node);
}

bool ParamTreeModel::canAddAttribute(const QModelIndex &index) const
{
    if (m_locked || m_doc.format != ParamFormat::Xml)
        return false;
    const ParamNode *node = nodeAt(index);
    if (!node)
        return false;
    if (node->kind == ParamKind::Element)
        return true;
    return node->kind == ParamKind::Attribute && node->parent && node->parent->kind == ParamKind::Element;
}

bool ParamTreeModel::addChild(const QModelIndex &index)
{
    if (m_locked || !m_doc.root)
        return false;
    ParamNode *selected = nodeAt(index);
    ParamNode *parent = m_doc.root.get();
    int row = parent->childCount();
    QModelIndex parentIndex;
    if (selected && containsChildren(selected)) {
        parent = selected;
        row = parent->childCount();
        parentIndex = index.sibling(index.row(), 0);
    } else if (selected && selected->parent) {
        parent = selected->parent;
        row = selected->rowInParent() + 1;
        parentIndex = index.parent();
    }
    auto created = makeInserted(parent);
    if (!created)
        return false;

    beginInsertRows(parentIndex, row, row);
    created->parent = parent;
    parent->children.insert(parent->children.begin() + row, std::move(created));
    if (parent->kind == ParamKind::Array)
        renumberArray(parent);
    endInsertRows();
    emit documentEdited();
    return true;
}

bool ParamTreeModel::addAttribute(const QModelIndex &index)
{
    if (!canAddAttribute(index))
        return false;
    ParamNode *selected = nodeAt(index);
    ParamNode *element = selected->kind == ParamKind::Element ? selected : selected->parent;
    int row = 0;
    while (row < element->childCount() && element->children[static_cast<size_t>(row)]->kind == ParamKind::Attribute)
        ++row;
    auto created = std::make_unique<ParamNode>();
    created->kind = ParamKind::Attribute;
    created->name = uniqueName(element, QStringLiteral("新属性"));
    const QModelIndex parentIndex = element == m_doc.root.get() ? QModelIndex() : indexOf(element, 0);
    beginInsertRows(parentIndex, row, row);
    created->parent = element;
    element->children.insert(element->children.begin() + row, std::move(created));
    endInsertRows();
    emit documentEdited();
    return true;
}

bool ParamTreeModel::removeNode(const QModelIndex &index)
{
    if (!canRemove(index))
        return false;
    ParamNode *node = nodeAt(index);
    ParamNode *parent = node->parent;
    const int row = node->rowInParent();
    beginRemoveRows(index.parent(), row, row);
    parent->children.erase(parent->children.begin() + row);
    if (parent->kind == ParamKind::Array)
        renumberArray(parent);
    endRemoveRows();
    emit documentEdited();
    return true;
}

bool ParamTreeModel::setScalarKind(const QModelIndex &index, ParamKind kind)
{
    ParamNode *node = nodeAt(index);
    if (m_locked || !node || !isJsonScalar(node->kind) || !isJsonScalar(kind))
        return false;
    if (node->kind == kind)
        return true;
    node->kind = kind;
    if (kind == ParamKind::Null) {
        node->value.clear();
        node->literal = QStringLiteral("null");
    } else if (kind == ParamKind::Bool) {
        node->value = node->value == QLatin1String("true") ? QStringLiteral("true") : QStringLiteral("false");
        node->literal = node->value;
    } else if (kind == ParamKind::Number) {
        if (!paramJsonNumberIsValid(node->value))
            node->value = QStringLiteral("0");
        node->literal = node->value;
    } else {
        node->literal.clear();
    }
    const QModelIndex left = index.sibling(index.row(), 0);
    emit dataChanged(left, index.sibling(index.row(), 2), {Qt::DisplayRole, Qt::EditRole});
    emit documentEdited();
    return true;
}

QVariant ParamTreeModel::data(const QModelIndex &index, int role) const
{
    const ParamNode *node = nodeAt(index);
    if (!node)
        return {};
    if (role == Qt::ToolTipRole && index.column() == 2)
        return paramNodeValueText(node);
    if (role != Qt::DisplayRole && role != Qt::EditRole)
        return {};
    switch (index.column()) {
    case 0:
        return paramNodeTitle(node);
    case 1:
        return paramKindLabel(node->kind);
    case 2:
        return paramNodeValueText(node);
    default:
        return {};
    }
}

bool ParamTreeModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (m_locked || role != Qt::EditRole || !index.isValid())
        return false;
    ParamNode *node = nodeAt(index);
    if (!node)
        return false;
    const QString text = value.toString();

    if (index.column() == 0) {
        if (!paramNodeTitleEditable(node))
            return false;
        QString name = text.trimmed();
        if (name.startsWith(QLatin1Char('@')))
            name = name.mid(1);
        if (name.isEmpty()) {
            emit editRejected(QStringLiteral("名称不能为空"));
            return false;
        }
        if ((node->kind == ParamKind::Element || node->kind == ParamKind::Attribute) && !xmlNameOk(name)) {
            emit editRejected(QStringLiteral("名称不是合法的 XML 名称"));
            return false;
        }
        if (node->kind == ParamKind::Section
            && (name.contains(QLatin1Char('[')) || name.contains(QLatin1Char(']')))) {
            emit editRejected(QStringLiteral("节名不能包含方括号"));
            return false;
        }
        if (node->kind == ParamKind::Key && (name.contains(QLatin1Char('=')) || name.contains(QLatin1Char(' ')))) {
            emit editRejected(QStringLiteral("键名不能包含空格或等号"));
            return false;
        }
        if (node->parent && (node->parent->kind == ParamKind::Object || node->kind == ParamKind::Attribute)) {
            for (const auto &sibling : node->parent->children) {
                if (sibling.get() != node && sibling->name == name
                    && (node->parent->kind == ParamKind::Object || sibling->kind == node->kind)) {
                    emit editRejected(QStringLiteral("同一层里已经有这个名称"));
                    return false;
                }
            }
        }
        node->name = name;
        emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
        emit documentEdited();
        return true;
    }

    if (index.column() != 2 || !paramNodeValueEditable(node))
        return false;

    if (node->kind == ParamKind::Number) {
        if (!paramJsonNumberIsValid(text)) {
            emit editRejected(QStringLiteral("数字格式不正确，例如 1、1.50 或 1e2"));
            return false;
        }
        node->value = text;
        node->literal = text;
    } else if (node->kind == ParamKind::Bool) {
        if (text != QLatin1String("true") && text != QLatin1String("false")) {
            emit editRejected(QStringLiteral("布尔值只能是 true 或 false"));
            return false;
        }
        node->value = text;
        node->literal = text;
    } else if (node->kind == ParamKind::Key) {
        if (text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r'))) {
            emit editRejected(QStringLiteral("INI 的值不能换行"));
            return false;
        }
        node->value = text;
    } else if (node->kind == ParamKind::Comment && m_doc.format == ParamFormat::Xml) {
        if (text.contains(QLatin1String("--"))) {
            emit editRejected(QStringLiteral("XML 注释里不能包含连续的连字符"));
            return false;
        }
        node->value = text;
    } else if (node->kind == ParamKind::Element) {
        int texts = 0;
        ParamNode *textNode = nullptr;
        for (const auto &child : node->children) {
            if (child->kind == ParamKind::Text) {
                ++texts;
                textNode = child.get();
            }
        }
        if (texts > 1)
            return false;
        if (!textNode) {
            if (text.isEmpty())
                return true;
            int row = 0;
            while (row < node->childCount()
                   && node->children[static_cast<size_t>(row)]->kind == ParamKind::Attribute)
                ++row;
            auto created = std::make_unique<ParamNode>();
            created->kind = ParamKind::Text;
            created->value = text;
            beginInsertRows(index.sibling(index.row(), 0), row, row);
            created->parent = node;
            node->children.insert(node->children.begin() + row, std::move(created));
            endInsertRows();
        } else {
            textNode->value = text;
            emit dataChanged(indexOf(textNode, 0), indexOf(textNode, 2), {Qt::DisplayRole, Qt::EditRole});
        }
    } else {
        node->value = text;
    }

    emit dataChanged(index.sibling(index.row(), 0), index.sibling(index.row(), 2), {Qt::DisplayRole, Qt::EditRole});
    emit documentEdited();
    return true;
}

QVariant ParamTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
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

QModelIndex ParamTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column < 0 || column >= 3)
        return {};
    ParamNode *container = parentNode(parent);
    if (!container || row < 0 || row >= container->childCount())
        return {};
    return createIndex(row, column, container->children[static_cast<size_t>(row)].get());
}

QModelIndex ParamTreeModel::parent(const QModelIndex &index) const
{
    const ParamNode *node = nodeAt(index);
    if (!node || !node->parent || node->parent == m_doc.root.get())
        return {};
    return createIndex(node->parent->rowInParent(), 0, node->parent);
}

int ParamTreeModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return 0;
    const ParamNode *node = parentNode(parent);
    return node ? node->childCount() : 0;
}

int ParamTreeModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 3;
}

Qt::ItemFlags ParamTreeModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!index.isValid() || m_locked)
        return flags;
    const ParamNode *node = nodeAt(index);
    if (!node)
        return flags;
    const bool editable = (index.column() == 0 && paramNodeTitleEditable(node))
        || (index.column() == 2 && paramNodeValueEditable(node));
    if (editable)
        flags |= Qt::ItemIsEditable;
    return flags;
}

} // namespace tw
