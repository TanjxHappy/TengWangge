#pragma once

#include <memory>
#include <vector>

#include <QString>
#include <QStringList>

namespace tw {

// 用户指定的格式。Automatic 只用于“自动识别”；解析成功后的 Sheet::notation
// 一定是 Json、Xml 或 Ini。
enum class Notation {
    Automatic,
    Json,
    Xml,
    Ini
};

enum class NodeKind {
    Document,
    Map,
    List,
    Phrase,
    Quantity,
    Switch,
    Vacant,
    Element,
    Attribute,
    Characters,
    Comment,
    Section,
    Field,
    Blank
};

// 一棵按源文件顺序排列的参数树。数字保留原始写法（例如 1.50）。
struct Node {
    NodeKind kind = NodeKind::Phrase;
    QString name;
    QString value;
    QString spelling;
    bool quoted = false;
    bool cdata = false;
    // 在原文中的位置。0 表示还没有对应行（例如结构刚改完、还没重新对上）。
    int line = 0;
    int column = 1;
    Node *parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;

    int childCount() const { return static_cast<int>(children.size()); }

    Node *append(std::unique_ptr<Node> child)
    {
        child->parent = this;
        Node *raw = child.get();
        children.push_back(std::move(child));
        return raw;
    }

    int indexInParent() const
    {
        if (!parent)
            return 0;
        for (int i = 0; i < parent->childCount(); ++i) {
            if (parent->children[static_cast<size_t>(i)].get() == this)
                return i;
        }
        return 0;
    }
};

struct Sheet {
    bool ok = false;
    Notation notation = Notation::Json;
    QString message;
    int line = 1;
    int column = 1;
    QString prologue;
    std::unique_ptr<Node> root;
};

QString notationCaption(Notation notation);
QString nodeKindCaption(NodeKind kind);
QString nodeLabel(const Node *node);
QString displayedValue(const Node *node);
bool isJsonNumber(const QString &text);
bool nodeNameEditable(const Node *node);
bool nodeValueEditable(const Node *node);
bool isJsonScalar(const Node *node);

// suffix 不含点。requested 为 Automatic 时按内容和后缀识别。
Sheet parseSheet(const QString &text, Notation requested, const QString &suffix = QString());
QString composeSheet(const Sheet &sheet, QString *error = nullptr);
Node *findNode(Node *root, const QStringList &path);

} // namespace tw
