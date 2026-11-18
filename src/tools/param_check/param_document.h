#pragma once

#include <memory>
#include <vector>

#include <QString>

namespace tw {

// 参数文件的格式。Auto / Unsupported 只出现在打开和识别阶段，
// 成功解析后的文档一定是 Json、Xml 或 Ini。
enum class ParamFormat {
    Auto,
    Json,
    Xml,
    Ini,
    Unsupported
};

enum class ParamKind {
    Document,
    Object,
    Array,
    String,
    Number,
    Bool,
    Null,
    Element,
    Attribute,
    Text,
    Comment,
    Section,
    Key,
    Instruction
};

// 一棵与格式无关的参数树。对象键、数组元素、XML 子节点都按源文件顺序存放。
struct ParamNode {
    ParamKind kind = ParamKind::String;
    QString name;
    QString value;
    // JSON 数字的原始写法（例如 1.50、1e2）。用户改过值之后与 value 相同。
    // INI 整行注释则用它记下分号或井号。
    QString literal;
    // INI 键或节的行尾注释，含开头的 ; 或 #。值本身不再带这段文字。
    QString inlineComment;
    bool cdata = false;
    bool quoted = false;
    ParamNode *parent = nullptr;
    std::vector<std::unique_ptr<ParamNode>> children;

    int childCount() const { return static_cast<int>(children.size()); }

    int rowInParent() const
    {
        if (!parent)
            return 0;
        for (int i = 0; i < parent->childCount(); ++i) {
            if (parent->children[static_cast<size_t>(i)].get() == this)
                return i;
        }
        return 0;
    }

    ParamNode *append(std::unique_ptr<ParamNode> child)
    {
        child->parent = this;
        ParamNode *raw = child.get();
        children.push_back(std::move(child));
        return raw;
    }
};

struct ParamDocument {
    bool ok = false;
    ParamFormat format = ParamFormat::Unsupported;
    QString error;
    int errorLine = 0;
    int errorColumn = 0;
    // 原文件里的 <?xml ...?>。树编辑后原样写回，不重新生成。
    QString xmlDeclaration;
    std::unique_ptr<ParamNode> root;
};

QString paramFormatName(ParamFormat format);
QString paramKindLabel(ParamKind kind);
QString paramNodeTitle(const ParamNode *node);
QString paramNodeValueText(const ParamNode *node);
bool paramNodeTitleEditable(const ParamNode *node);
bool paramNodeValueEditable(const ParamNode *node);
bool paramJsonNumberIsValid(const QString &text);

// suffix 不含点，用于自动识别；用户指定格式时忽略它。
ParamFormat detectParamFormat(const QString &text, const QString &suffix);
ParamDocument parseParamText(const QString &text, ParamFormat format, const QString &suffix = QString());
QString serializeParam(const ParamDocument &document, QString *error = nullptr);

} // namespace tw
