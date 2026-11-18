#include "tools/param_check/param_document.h"

#include <QDomDocument>
#include <QDomProcessingInstruction>
#include <QSet>
#include <QStringList>

namespace tw {
namespace {

ParamDocument failed(ParamFormat format, const QString &error, int line = 0, int column = 0)
{
    ParamDocument document;
    document.ok = false;
    document.format = format;
    document.error = error;
    document.errorLine = line;
    document.errorColumn = column;
    return document;
}

QString lineMessage(int line, int column, const QString &message)
{
    return QStringLiteral("第 %1 行第 %2 个字符：%3").arg(line).arg(column).arg(message);
}

// 超大文件会占满内存。测试可以把上限调低，正常使用走默认值。
int envLimit(const char *name, int fallback, int minValue, int maxValue)
{
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(name, &ok);
    if (!ok || value < minValue || value > maxValue)
        return fallback;
    return value;
}

int paramMaxNodes()
{
    return envLimit("TW_PARAM_MAX_NODES", 200000, 8, 5000000);
}

int paramMaxChars()
{
    return envLimit("TW_PARAM_MAX_CHARS", 16000000, 8, 200000000);
}

constexpr int kMaxParamDepth = 128;

QString tooDeepMessage()
{
    return QStringLiteral("嵌套超过 %1 层，已停止解析，以免把界面卡住。请把结构拆浅一些。").arg(kMaxParamDepth);
}

QString tooManyNodesMessage(int limit)
{
    return QStringLiteral("内容太多（超过 %1 项），已停止解析，以免把界面卡住。可以把文件拆小后再检查。")
        .arg(limit);
}

bool looksLikeIniSection(const QString &trimmed)
{
    if (!trimmed.startsWith(QLatin1Char('[')))
        return false;
    int index = 1;
    while (index < trimmed.size() && trimmed.at(index).isSpace())
        ++index;
    if (index >= trimmed.size() || trimmed.at(index) == QLatin1Char(']'))
        return false;
    const QChar next = trimmed.at(index);
    // JSON 数组在左方括号后面是值：字符串、数字、对象、数组或 true/false/null。
    if (next == QLatin1Char('"') || next == QLatin1Char('{') || next == QLatin1Char('[') || next == QLatin1Char('-')
        || next.isDigit() || next == QLatin1Char('t') || next == QLatin1Char('f') || next == QLatin1Char('n'))
        return false;
    return trimmed.indexOf(QLatin1Char(']')) > index;
}

bool looksLikeIni(const QString &text)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char(';')) || trimmed.startsWith(QLatin1Char('#')))
            continue;
        if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']')))
            return true;
        if (trimmed.contains(QLatin1Char('=')))
            return true;
    }
    return false;
}

int countKind(const ParamNode *node, ParamKind kind)
{
    int count = 0;
    if (!node)
        return 0;
    for (const auto &child : node->children) {
        if (child->kind == kind)
            ++count;
    }
    return count;
}

ParamNode *onlyTextChild(const ParamNode *node)
{
    ParamNode *found = nullptr;
    for (const auto &child : node->children) {
        if (child->kind != ParamKind::Text)
            continue;
        if (found)
            return nullptr;
        found = child.get();
    }
    return found;
}

class JsonParser {
public:
    explicit JsonParser(const QString &text)
        : m_text(text)
    {
    }

    ParamDocument parse()
    {
        m_maxNodes = paramMaxNodes();
        auto value = parseValue();
        if (!value) {
            if (m_error.isEmpty())
                setError(QStringLiteral("JSON 没有解析出内容。"));
            return failed(ParamFormat::Json, m_error, m_errorLine, m_errorColumn);
        }
        skipWs();
        if (!eof()) {
            setError(QStringLiteral("这里已经结束了，后面还有多余内容。请删掉多出来的文字，或检查括号是否提前闭合。"));
            return failed(ParamFormat::Json, m_error, m_errorLine, m_errorColumn);
        }

        ParamDocument document;
        document.ok = true;
        document.format = ParamFormat::Json;
        if (value->kind == ParamKind::Object || value->kind == ParamKind::Array) {
            document.root = std::move(value);
        } else {
            document.root = std::make_unique<ParamNode>();
            document.root->kind = ParamKind::Document;
            value->name = QStringLiteral("值");
            document.root->append(std::move(value));
        }
        return document;
    }

private:
    const QString &m_text;
    int m_index = 0;
    int m_line = 1;
    int m_column = 1;
    int m_depth = 0;
    int m_nodes = 0;
    int m_maxNodes = 200000;
    QString m_error;
    int m_errorLine = 1;
    int m_errorColumn = 1;

    bool eof() const { return m_index >= m_text.size(); }
    QChar ch() const { return eof() ? QChar() : m_text.at(m_index); }

    void advance()
    {
        if (eof())
            return;
        if (m_text.at(m_index) == QLatin1Char('\n')) {
            ++m_line;
            m_column = 1;
        } else {
            ++m_column;
        }
        ++m_index;
    }

    void skipWs()
    {
        while (!eof() && ch().isSpace())
            advance();
    }

    void setError(const QString &message)
    {
        if (!m_error.isEmpty())
            return;
        m_errorLine = m_line;
        m_errorColumn = m_column;
        m_error = lineMessage(m_line, m_column, message);
    }

    class DepthGuard {
    public:
        explicit DepthGuard(int *depth)
            : m_depth(depth)
        {
            ++(*m_depth);
        }
        ~DepthGuard() { --(*m_depth); }

    private:
        int *m_depth;
    };

    bool takeNode()
    {
        if (++m_nodes <= m_maxNodes)
            return true;
        setError(tooManyNodesMessage(m_maxNodes));
        return false;
    }

    std::unique_ptr<ParamNode> parseValue()
    {
        if (m_depth >= kMaxParamDepth) {
            setError(tooDeepMessage());
            return nullptr;
        }
        DepthGuard guard(&m_depth);
        skipWs();
        if (eof()) {
            setError(QStringLiteral("这里还缺一个值。可以填写文字、数字、true、false、null，或者一对括号。"));
            return nullptr;
        }
        const QChar c = ch();
        if (c == QLatin1Char('{'))
            return parseObject();
        if (c == QLatin1Char('['))
            return parseArray();
        if (c == QLatin1Char('"'))
            return parseStringValue();
        if (c == QLatin1Char('t'))
            return parseLiteral(QStringLiteral("true"), ParamKind::Bool, QStringLiteral("true"));
        if (c == QLatin1Char('f'))
            return parseLiteral(QStringLiteral("false"), ParamKind::Bool, QStringLiteral("false"));
        if (c == QLatin1Char('n'))
            return parseLiteral(QStringLiteral("null"), ParamKind::Null, QString());
        if (c == QLatin1Char('-') || c.isDigit())
            return parseNumber();
        setError(QStringLiteral("这里的内容无法识别。可以填写文字、数字、true、false、null，或者对象和数组。"));
        return nullptr;
    }

    std::unique_ptr<ParamNode> parseObject()
    {
        advance(); // {
        auto object = std::make_unique<ParamNode>();
        object->kind = ParamKind::Object;
        if (!takeNode())
            return nullptr;
        skipWs();
        if (ch() == QLatin1Char('}')) {
            advance();
            return object;
        }
        QSet<QString> seen;
        while (!eof()) {
            skipWs();
            if (ch() != QLatin1Char('"')) {
                setError(QStringLiteral("对象的名称必须用英文双引号括起来，例如 \"端口\"。"));
                return nullptr;
            }
            QString key;
            if (!readString(&key))
                return nullptr;
            if (seen.contains(key)) {
                setError(QStringLiteral("重复的键「") + key
                         + QStringLiteral("」。同一个对象里每个名称只能出现一次。"));
                return nullptr;
            }
            seen.insert(key);
            skipWs();
            if (ch() != QLatin1Char(':')) {
                setError(QStringLiteral("名称后面缺少冒号。正确写法类似 \"端口\": 8080。"));
                return nullptr;
            }
            advance();
            auto child = parseValue();
            if (!child)
                return nullptr;
            child->name = key;
            object->append(std::move(child));
            skipWs();
            if (ch() == QLatin1Char(',')) {
                advance();
                skipWs();
                if (ch() == QLatin1Char('}')) {
                    setError(QStringLiteral("最后一项后面多了一个逗号。请把这个逗号删掉。"));
                    return nullptr;
                }
                continue;
            }
            if (ch() == QLatin1Char('}')) {
                advance();
                return object;
            }
            setError(QStringLiteral("对象还没有写完。请检查逗号，以及右边的 } 是否配对。"));
            return nullptr;
        }
        setError(QStringLiteral("对象少了右边的 }。"));
        return nullptr;
    }

    std::unique_ptr<ParamNode> parseArray()
    {
        advance(); // [
        auto array = std::make_unique<ParamNode>();
        array->kind = ParamKind::Array;
        if (!takeNode())
            return nullptr;
        skipWs();
        if (ch() == QLatin1Char(']')) {
            advance();
            return array;
        }
        while (!eof()) {
            auto child = parseValue();
            if (!child)
                return nullptr;
            child->name = QStringLiteral("[%1]").arg(array->childCount());
            array->append(std::move(child));
            skipWs();
            if (ch() == QLatin1Char(',')) {
                advance();
                skipWs();
                if (ch() == QLatin1Char(']')) {
                    setError(QStringLiteral("最后一项后面多了一个逗号。请把这个逗号删掉。"));
                    return nullptr;
                }
                continue;
            }
            if (ch() == QLatin1Char(']')) {
                advance();
                return array;
            }
            setError(QStringLiteral("数组还没有写完。请检查逗号，以及右边的 ] 是否配对。"));
            return nullptr;
        }
        setError(QStringLiteral("数组少了右边的 ]。"));
        return nullptr;
    }

    std::unique_ptr<ParamNode> parseStringValue()
    {
        QString text;
        if (!readString(&text))
            return nullptr;
        auto node = std::make_unique<ParamNode>();
        node->kind = ParamKind::String;
        if (!takeNode())
            return nullptr;
        node->value = text;
        return node;
    }

    bool readString(QString *out)
    {
        if (ch() != QLatin1Char('"')) {
            setError(QStringLiteral("这里应该是用英文双引号括起来的文字。"));
            return false;
        }
        advance();
        QString value;
        while (!eof()) {
            const QChar c = ch();
            if (c == QLatin1Char('"')) {
                advance();
                *out = value;
                return true;
            }
            if (c == QLatin1Char('\\')) {
                advance();
                if (eof()) {
                    setError(QStringLiteral("引号里的反斜杠不完整。例如引号写成 \\\"，换行写成 \\n。"));
                    return false;
                }
                const QChar esc = ch();
                advance();
                switch (esc.unicode()) {
                case '"':
                case '\\':
                case '/':
                    value += esc;
                    break;
                case 'b':
                    value += QLatin1Char('\b');
                    break;
                case 'f':
                    value += QLatin1Char('\f');
                    break;
                case 'n':
                    value += QLatin1Char('\n');
                    break;
                case 'r':
                    value += QLatin1Char('\r');
                    break;
                case 't':
                    value += QLatin1Char('\t');
                    break;
                case 'u':
                    if (!readUnicodeEscape(&value))
                        return false;
                    break;
                default:
                    setError(QStringLiteral("引号里有无法识别的转义。可以使用 \\\"、\\\\、\\n、\\t 和 \\u 加四位十六进制。"));
                    return false;
                }
                continue;
            }
            if (c.unicode() < 0x20) {
                setError(QStringLiteral("引号里面有换行或控制字符。请把换行写成 \\n，或者改成一行。"));
                return false;
            }
            if (c.isHighSurrogate()) {
                value += c;
                advance();
                if (eof() || !ch().isLowSurrogate()) {
                    setError(QStringLiteral("文字里有不完整的特殊字符，无法可靠保存。请删掉这段异常字符。"));
                    return false;
                }
                value += ch();
                advance();
                continue;
            }
            if (c.isLowSurrogate()) {
                setError(QStringLiteral("文字里有不完整的特殊字符，无法可靠保存。请删掉这段异常字符。"));
                return false;
            }
            value += c;
            advance();
        }
        setError(QStringLiteral("这段文字少了结束的英文双引号。"));
        return false;
    }

    bool readUnicodeEscape(QString *value)
    {
        int code = 0;
        if (!readHex4(&code))
            return false;
        if (code >= 0xDC00 && code <= 0xDFFF) {
            setError(QStringLiteral("文字里的 \\u 编码不成对。请检查这处四位编码。"));
            return false;
        }
        if (code >= 0xD800 && code <= 0xDBFF) {
            if (ch() != QLatin1Char('\\')) {
                setError(QStringLiteral("文字里的 \\u 编码不成对。高位和低位要连在一起写。"));
                return false;
            }
            advance();
            if (ch() != QLatin1Char('u')) {
                setError(QStringLiteral("文字里的 \\u 编码不成对。高位和低位要连在一起写。"));
                return false;
            }
            advance();
            int low = 0;
            if (!readHex4(&low))
                return false;
            if (low < 0xDC00 || low > 0xDFFF) {
                setError(QStringLiteral("文字里的 \\u 编码不成对。高位和低位要连在一起写。"));
                return false;
            }
            const uint point = 0x10000u + ((static_cast<uint>(code) - 0xD800u) << 10)
                + (static_cast<uint>(low) - 0xDC00u);
            value->append(QString::fromUcs4(&point, 1));
            return true;
        }
        value->append(QChar(static_cast<ushort>(code)));
        return true;
    }

    bool readHex4(int *code)
    {
        int value = 0;
        for (int i = 0; i < 4; ++i) {
            if (eof()) {
                setError(QStringLiteral("\\u 后面需要正好 4 位十六进制数字，例如 \\u6ed5。"));
                return false;
            }
            const QChar c = ch();
            int digit = -1;
            if (c.isDigit())
                digit = c.digitValue();
            else if (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
                digit = 10 + c.unicode() - 'a';
            else if (c >= QLatin1Char('A') && c <= QLatin1Char('F'))
                digit = 10 + c.unicode() - 'A';
            if (digit < 0) {
                setError(QStringLiteral("\\u 后面需要正好 4 位十六进制数字，例如 \\u6ed5。"));
                return false;
            }
            value = (value << 4) + digit;
            advance();
        }
        *code = value;
        return true;
    }

    std::unique_ptr<ParamNode> parseLiteral(const QString &word, ParamKind kind, const QString &value)
    {
        const int savedIndex = m_index;
        const int savedLine = m_line;
        const int savedColumn = m_column;
        for (int i = 0; i < word.size(); ++i) {
            if (eof() || ch() != word.at(i)) {
                m_index = savedIndex;
                m_line = savedLine;
                m_column = savedColumn;
                setError(QStringLiteral("这里的内容无法识别。可以填写文字、数字、true、false、null，或者对象和数组。"));
                return nullptr;
            }
            advance();
        }
        if (!eof() && (ch().isLetterOrNumber() || ch() == QLatin1Char('_'))) {
            setError(QStringLiteral("这里的内容无法识别。可以填写文字、数字、true、false、null，或者对象和数组。"));
            return nullptr;
        }
        auto node = std::make_unique<ParamNode>();
        node->kind = kind;
        if (!takeNode())
            return nullptr;
        node->value = value;
        node->literal = word;
        return node;
    }

    std::unique_ptr<ParamNode> parseNumber()
    {
        const int start = m_index;
        if (ch() == QLatin1Char('-'))
            advance();
        if (eof() || !ch().isDigit()) {
            setError(QStringLiteral("这个数字不完整。"));
            return nullptr;
        }
        if (ch() == QLatin1Char('0')) {
            advance();
            if (!eof() && ch().isDigit()) {
                setError(QStringLiteral("数字不能以 0 开头。例如请把 01 写成 1；小数请写成 0.1。"));
                return nullptr;
            }
        } else {
            while (!eof() && ch().isDigit())
                advance();
        }
        if (ch() == QLatin1Char('.')) {
            advance();
            if (eof() || !ch().isDigit()) {
                setError(QStringLiteral("小数点后面还缺数字，例如 1.5。"));
                return nullptr;
            }
            while (!eof() && ch().isDigit())
                advance();
        }
        if (ch() == QLatin1Char('e') || ch() == QLatin1Char('E')) {
            advance();
            if (ch() == QLatin1Char('+') || ch() == QLatin1Char('-'))
                advance();
            if (eof() || !ch().isDigit()) {
                setError(QStringLiteral("科学计数法的指数不完整，例如 1e2。"));
                return nullptr;
            }
            while (!eof() && ch().isDigit())
                advance();
        }
        auto node = std::make_unique<ParamNode>();
        node->kind = ParamKind::Number;
        if (!takeNode())
            return nullptr;
        node->literal = m_text.mid(start, m_index - start);
        node->value = node->literal;
        return node;
    }
};

QString escapeJsonString(const QString &text)
{
    QString out;
    out.reserve(text.size() + 2);
    out += QLatin1Char('"');
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '"':
            out += QLatin1String("\\\"");
            break;
        case '\\':
            out += QLatin1String("\\\\");
            break;
        case '\b':
            out += QLatin1String("\\b");
            break;
        case '\f':
            out += QLatin1String("\\f");
            break;
        case '\n':
            out += QLatin1String("\\n");
            break;
        case '\r':
            out += QLatin1String("\\r");
            break;
        case '\t':
            out += QLatin1String("\\t");
            break;
        default:
            if (c.unicode() < 0x20)
                out += QStringLiteral("\\u%1").arg(c.unicode(), 4, 16, QLatin1Char('0'));
            else
                out += c;
            break;
        }
    }
    out += QLatin1Char('"');
    return out;
}

void appendIndent(QString *out, int spaces)
{
    out->append(QString(spaces, QLatin1Char(' ')));
}

void writeJsonValue(QString *out, const ParamNode &node, int indent)
{
    switch (node.kind) {
    case ParamKind::Object: {
        if (node.childCount() == 0) {
            out->append(QLatin1String("{}"));
            return;
        }
        out->append(QLatin1String("{\n"));
        for (int i = 0; i < node.childCount(); ++i) {
            const ParamNode *child = node.children[static_cast<size_t>(i)].get();
            appendIndent(out, indent + 2);
            *out += escapeJsonString(child->name);
            *out += QLatin1String(": ");
            writeJsonValue(out, *child, indent + 2);
            if (i + 1 != node.childCount())
                out->append(QLatin1Char(','));
            out->append(QLatin1Char('\n'));
        }
        appendIndent(out, indent);
        out->append(QLatin1Char('}'));
        return;
    }
    case ParamKind::Array: {
        if (node.childCount() == 0) {
            out->append(QLatin1String("[]"));
            return;
        }
        out->append(QLatin1String("[\n"));
        for (int i = 0; i < node.childCount(); ++i) {
            appendIndent(out, indent + 2);
            writeJsonValue(out, *node.children[static_cast<size_t>(i)], indent + 2);
            if (i + 1 != node.childCount())
                out->append(QLatin1Char(','));
            out->append(QLatin1Char('\n'));
        }
        appendIndent(out, indent);
        out->append(QLatin1Char(']'));
        return;
    }
    case ParamKind::String:
        *out += escapeJsonString(node.value);
        return;
    case ParamKind::Number:
        *out += node.literal.isEmpty() ? node.value : node.literal;
        return;
    case ParamKind::Bool:
        *out += node.value == QLatin1String("true") ? QLatin1String("true") : QLatin1String("false");
        return;
    case ParamKind::Null:
        out->append(QLatin1String("null"));
        return;
    case ParamKind::Document:
        if (node.childCount() == 1)
            writeJsonValue(out, *node.children.front(), indent);
        else
            out->append(QLatin1String("null"));
        return;
    default:
        out->append(QLatin1String("null"));
        return;
    }
}

QString serializeJson(const ParamNode &root)
{
    QString out;
    writeJsonValue(&out, root, 0);
    out += QLatin1Char('\n');
    return out;
}

QString captureDoctype(const QString &text)
{
    const int start = text.indexOf(QLatin1String("<!DOCTYPE"), 0, Qt::CaseInsensitive);
    if (start < 0)
        return {};
    int depth = 1;
    bool inQuote = false;
    QChar quote;
    for (int i = start + 9; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (inQuote) {
            if (c == quote)
                inQuote = false;
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            inQuote = true;
            quote = c;
            continue;
        }
        if (c == QLatin1Char('['))
            ++depth;
        else if (c == QLatin1Char(']') && depth > 1)
            --depth;
        else if (c == QLatin1Char('>') && depth == 1)
            return text.mid(start, i - start + 1);
    }
    return {};
}

struct XmlBuild {
    int nodes = 0;
    int limit = 0;
    QString error;
};

bool takeXmlNode(XmlBuild *build)
{
    if (!build->error.isEmpty())
        return false;
    if (++build->nodes <= build->limit)
        return true;
    build->error = tooManyNodesMessage(build->limit);
    return false;
}

std::unique_ptr<ParamNode> makeXmlLeaf(ParamKind kind, XmlBuild *build)
{
    if (!takeXmlNode(build))
        return nullptr;
    auto node = std::make_unique<ParamNode>();
    node->kind = kind;
    return node;
}

std::unique_ptr<ParamNode> convertXmlNode(const QDomNode &domNode, int depth, XmlBuild *build)
{
    if (!build->error.isEmpty())
        return nullptr;
    if (domNode.isText() && !domNode.isCDATASection() && domNode.nodeValue().trimmed().isEmpty())
        return nullptr;
    if (depth > kMaxParamDepth) {
        build->error = tooDeepMessage();
        return nullptr;
    }

    if (domNode.isElement()) {
        const QDomElement element = domNode.toElement();
        auto node = makeXmlLeaf(ParamKind::Element, build);
        if (!node)
            return nullptr;
        node->name = element.tagName();
        const QDomNamedNodeMap attributes = element.attributes();
        for (int i = 0; i < attributes.count(); ++i) {
            const QDomAttr attr = attributes.item(i).toAttr();
            auto child = makeXmlLeaf(ParamKind::Attribute, build);
            if (!child)
                return nullptr;
            child->name = attr.name();
            child->value = attr.value();
            node->append(std::move(child));
        }
        for (QDomNode child = element.firstChild(); !child.isNull(); child = child.nextSibling()) {
            auto converted = convertXmlNode(child, depth + 1, build);
            if (!build->error.isEmpty())
                return nullptr;
            if (converted)
                node->append(std::move(converted));
        }
        return node;
    }
    if (domNode.isCDATASection()) {
        auto node = makeXmlLeaf(ParamKind::Text, build);
        if (!node)
            return nullptr;
        node->value = domNode.nodeValue();
        node->cdata = true;
        return node;
    }
    if (domNode.isText()) {
        auto node = makeXmlLeaf(ParamKind::Text, build);
        if (!node)
            return nullptr;
        node->value = domNode.nodeValue();
        return node;
    }
    if (domNode.isComment()) {
        auto node = makeXmlLeaf(ParamKind::Comment, build);
        if (!node)
            return nullptr;
        node->value = domNode.nodeValue();
        return node;
    }
    if (domNode.isProcessingInstruction()) {
        const QDomProcessingInstruction pi = domNode.toProcessingInstruction();
        auto node = makeXmlLeaf(ParamKind::Instruction, build);
        if (!node)
            return nullptr;
        node->name = pi.target();
        node->value = pi.data();
        return node;
    }
    return nullptr;
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

bool appendXml(QDomDocument *dom, QDomNode parent, const ParamNode &node, QString *error)
{
    switch (node.kind) {
    case ParamKind::Element: {
        if (!xmlNameOk(node.name)) {
            if (error)
                *error = QStringLiteral("元素名「%1」不能写入 XML。请改用字母开头的名称。").arg(node.name);
            return false;
        }
        QDomElement element = dom->createElement(node.name);
        parent.appendChild(element);
        for (const auto &child : node.children) {
            if (child->kind != ParamKind::Attribute)
                continue;
            if (!xmlNameOk(child->name)) {
                if (error)
                    *error = QStringLiteral("属性名「%1」不能写入 XML。请改用字母开头的名称。").arg(child->name);
                return false;
            }
            element.setAttribute(child->name, child->value);
        }
        for (const auto &child : node.children) {
            if (child->kind == ParamKind::Attribute)
                continue;
            if (!appendXml(dom, element, *child, error))
                return false;
        }
        return true;
    }
    case ParamKind::Text:
        if (node.cdata)
            parent.appendChild(dom->createCDATASection(node.value));
        else
            parent.appendChild(dom->createTextNode(node.value));
        return true;
    case ParamKind::Comment:
        if (node.value.contains(QLatin1String("--"))) {
            if (error)
                *error = QStringLiteral("注释里不能连续写两个减号（--），否则无法保存为 XML。");
            return false;
        }
        parent.appendChild(dom->createComment(node.value));
        return true;
    case ParamKind::Instruction:
        if (node.name == QLatin1String("!DOCTYPE"))
            return true;
        parent.appendChild(dom->createProcessingInstruction(node.name, node.value));
        return true;
    default:
        if (error)
            *error = QStringLiteral("结构里有这种格式不能保存的内容。请改左侧文本，或删掉这项后再保存。");
        return false;
    }
}

QString serializeXml(const ParamDocument &document, QString *error)
{
    // 默认构造的 QDomDocument 是空节点，createElement / appendChild 都会被丢掉。
    // 先放一个占位根再删掉，得到一个真正的空文档，又不会带上多余的文档类型。
    QDomDocument dom;
    if (!dom.setContent(QStringLiteral("<placeholder/>"))) {
        if (error)
            *error = QStringLiteral("无法创建 XML 文档");
        return {};
    }
    dom.removeChild(dom.firstChild());
    QStringList preface;
    if (!document.xmlDeclaration.isEmpty())
        preface.append(document.xmlDeclaration);
    if (document.root) {
        for (const auto &child : document.root->children) {
            if (child->kind == ParamKind::Instruction && child->name == QLatin1String("!DOCTYPE")) {
                preface.append(child->value);
                continue;
            }
            if (!appendXml(&dom, dom, *child, error))
                return {};
        }
    }
    QString body = dom.toString(2);
    if (!preface.isEmpty())
        body.prepend(preface.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    if (!body.endsWith(QLatin1Char('\n')))
        body += QLatin1Char('\n');
    return body;
}

QString escapeIniQuoted(const QString &text)
{
    QString out;
    for (const QChar c : text) {
        if (c == QLatin1Char('\\') || c == QLatin1Char('"'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

bool iniValueNeedsQuotes(const QString &value)
{
    return value.startsWith(QLatin1Char(' ')) || value.endsWith(QLatin1Char(' '))
        || value.contains(QLatin1Char('"')) || value.contains(QLatin1Char(';'))
        || value.contains(QLatin1Char('#'));
}

void writeIniNode(QString *out, const ParamNode &node)
{
    if (node.kind == ParamKind::Comment) {
        const QString marker = node.literal.isEmpty() ? QStringLiteral(";") : node.literal;
        *out += marker;
        if (!node.value.isEmpty())
            *out += QLatin1Char(' ') + node.value;
        *out += QLatin1Char('\n');
        return;
    }
    if (node.kind == ParamKind::Key) {
        *out += node.name;
        *out += QLatin1Char('=');
        if (node.quoted || iniValueNeedsQuotes(node.value))
            *out += QLatin1Char('"') + escapeIniQuoted(node.value) + QLatin1Char('"');
        else
            *out += node.value;
        if (!node.inlineComment.isEmpty())
            *out += QLatin1Char(' ') + node.inlineComment;
        *out += QLatin1Char('\n');
    }
}

QString serializeIni(const ParamNode &root)
{
    QString out;
    for (const auto &child : root.children) {
        if (child->kind == ParamKind::Section) {
            out += QLatin1Char('[') + child->name + QLatin1Char(']');
            if (!child->inlineComment.isEmpty())
                out += QLatin1Char(' ') + child->inlineComment;
            out += QLatin1Char('\n');
            for (const auto &entry : child->children)
                writeIniNode(&out, *entry);
            if (!out.endsWith(QLatin1String("\n\n")))
                out += QLatin1Char('\n');
            continue;
        }
        writeIniNode(&out, *child);
    }
    if (!out.endsWith(QLatin1Char('\n')))
        out += QLatin1Char('\n');
    return out;
}

// 行尾注释必须前面有空白，并且值里已经有内容。这样 port=8080 ; 监听 会拆开，
// 而 C:\a;b、#fff 这类值保持原样。
struct IniAssignment {
    bool ok = true;
    QString error;
    QString value;
    bool quoted = false;
    QString inlineComment;
};

IniAssignment splitIniAssignment(const QString &raw)
{
    IniAssignment result;
    int start = 0;
    while (start < raw.size() && raw.at(start).isSpace())
        ++start;
    if (start >= raw.size())
        return result;

    if (raw.at(start) == QLatin1Char('"')) {
        QString decoded;
        int index = start + 1;
        bool closed = false;
        while (index < raw.size()) {
            const QChar current = raw.at(index);
            if (current == QLatin1Char('\\') && index + 1 < raw.size()) {
                const QChar next = raw.at(index + 1);
                if (next == QLatin1Char('\\') || next == QLatin1Char('"')) {
                    decoded += next;
                    index += 2;
                    continue;
                }
            }
            if (current == QLatin1Char('"')) {
                closed = true;
                ++index;
                break;
            }
            decoded += current;
            ++index;
        }
        if (!closed) {
            result.ok = false;
            result.error = QStringLiteral("引号没有结束。值里如果有空格或分号，请用成对的双引号括起来。");
            return result;
        }
        int tail = index;
        while (tail < raw.size() && raw.at(tail).isSpace())
            ++tail;
        if (tail < raw.size()) {
            const QChar mark = raw.at(tail);
            if (mark == QLatin1Char(';') || mark == QLatin1Char('#')) {
                result.inlineComment = raw.mid(tail).trimmed();
            } else {
                result.ok = false;
                result.error = QStringLiteral("引号结束后还有多余内容。行尾说明请用 ; 或 # 开头。");
                return result;
            }
        }
        result.value = decoded;
        result.quoted = true;
        return result;
    }

    int commentAt = -1;
    for (int index = start + 1; index < raw.size(); ++index) {
        const QChar current = raw.at(index);
        if (current != QLatin1Char(';') && current != QLatin1Char('#'))
            continue;
        if (raw.at(index - 1).isSpace()) {
            commentAt = index;
            break;
        }
    }
    if (commentAt < 0) {
        result.value = raw.trimmed();
        return result;
    }
    result.value = raw.left(commentAt).trimmed();
    result.inlineComment = raw.mid(commentAt).trimmed();
    return result;
}

bool appendIniNode(ParamNode *parent, std::unique_ptr<ParamNode> node, int *count, int limit, QString *error)
{
    if (++(*count) > limit) {
        if (error)
            *error = tooManyNodesMessage(limit);
        return false;
    }
    parent->append(std::move(node));
    return true;
}

ParamDocument parseIni(const QString &text)
{
    ParamDocument document;
    document.ok = true;
    document.format = ParamFormat::Ini;
    document.root = std::make_unique<ParamNode>();
    document.root->kind = ParamKind::Document;
    ParamNode *section = document.root.get();
    const int nodeLimit = paramMaxNodes();
    int nodes = 1;

    int lineStart = 0;
    int lineNo = 1;
    while (lineStart <= text.size()) {
        int lineEnd = text.indexOf(QLatin1Char('\n'), lineStart);
        if (lineEnd < 0)
            lineEnd = text.size();
        QString line = text.mid(lineStart, lineEnd - lineStart);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            // 空行跳过，继续下一行。
        } else if (trimmed.startsWith(QLatin1Char(';')) || trimmed.startsWith(QLatin1Char('#'))) {
            auto comment = std::make_unique<ParamNode>();
            comment->kind = ParamKind::Comment;
            comment->literal = trimmed.left(1);
            comment->value = trimmed.mid(1).trimmed();
            QString error;
            if (!appendIniNode(section, std::move(comment), &nodes, nodeLimit, &error))
                return failed(ParamFormat::Ini, error, lineNo, 1);
        } else if (trimmed.startsWith(QLatin1Char('['))) {
            const int close = trimmed.indexOf(QLatin1Char(']'));
            if (close <= 1) {
                return failed(ParamFormat::Ini,
                              QStringLiteral("第 %1 行：节名需要用方括号包起来，例如 [server]。").arg(lineNo),
                              lineNo, 1);
            }
            const QString name = trimmed.mid(1, close - 1).trimmed();
            if (name.isEmpty() || name.contains(QLatin1Char('[')) || name.contains(QLatin1Char(']'))) {
                return failed(ParamFormat::Ini,
                              QStringLiteral("第 %1 行：节名无效。请去掉空名称或多余的方括号。").arg(lineNo),
                              lineNo, 1);
            }
            QString after = trimmed.mid(close + 1);
            int mark = 0;
            while (mark < after.size() && after.at(mark).isSpace())
                ++mark;
            QString inlineComment;
            if (mark < after.size()) {
                const QChar marker = after.at(mark);
                if (marker != QLatin1Char(';') && marker != QLatin1Char('#')) {
                    return failed(ParamFormat::Ini,
                                  QStringLiteral("第 %1 行：节名后面有多余内容。行尾说明请用 ; 或 # 开头。")
                                      .arg(lineNo),
                                  lineNo, 1);
                }
                inlineComment = after.mid(mark).trimmed();
            }
            auto created = std::make_unique<ParamNode>();
            created->kind = ParamKind::Section;
            created->name = name;
            created->inlineComment = inlineComment;
            QString error;
            if (!appendIniNode(document.root.get(), std::move(created), &nodes, nodeLimit, &error))
                return failed(ParamFormat::Ini, error, lineNo, 1);
            section = document.root->children.back().get();
        } else {
            const int eq = trimmed.indexOf(QLatin1Char('='));
            if (eq <= 0) {
                return failed(ParamFormat::Ini,
                              QStringLiteral("第 %1 行：这一行不是「键=值」，也不是节或注释。例如 port=8080。")
                                  .arg(lineNo),
                              lineNo, 1);
            }
            const QString key = trimmed.left(eq).trimmed();
            if (key.isEmpty() || key.contains(QLatin1Char(' ')) || key.contains(QLatin1Char('\t'))) {
                return failed(ParamFormat::Ini,
                              QStringLiteral("第 %1 行：键名不能为空，也不能包含空格。").arg(lineNo), lineNo, 1);
            }
            const IniAssignment assignment = splitIniAssignment(trimmed.mid(eq + 1));
            if (!assignment.ok) {
                return failed(ParamFormat::Ini, QStringLiteral("第 %1 行：%2").arg(lineNo).arg(assignment.error),
                              lineNo, eq + 2);
            }
            auto entry = std::make_unique<ParamNode>();
            entry->kind = ParamKind::Key;
            entry->name = key;
            entry->value = assignment.value;
            entry->quoted = assignment.quoted;
            entry->inlineComment = assignment.inlineComment;
            QString error;
            if (!appendIniNode(section, std::move(entry), &nodes, nodeLimit, &error))
                return failed(ParamFormat::Ini, error, lineNo, 1);
        }

        if (lineEnd >= text.size())
            break;
        lineStart = lineEnd + 1;
        ++lineNo;
    }
    return document;
}

bool xmlNestingExceeds(const QString &text, int limit)
{
    int depth = 0;
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) != QLatin1Char('<'))
            continue;
        if (i + 1 >= text.size())
            return false;
        const QChar next = text.at(i + 1);
        if (next == QLatin1Char('!')) {
            if (i + 3 < text.size() && text.at(i + 2) == QLatin1Char('-') && text.at(i + 3) == QLatin1Char('-')) {
                const int end = text.indexOf(QLatin1String("-->"), i + 4);
                if (end < 0)
                    return false;
                i = end + 2;
                continue;
            }
            if (text.mid(i, 9) == QLatin1String("<![CDATA[")) {
                const int end = text.indexOf(QLatin1String("]]>"), i + 9);
                if (end < 0)
                    return false;
                i = end + 2;
                continue;
            }
            const int end = text.indexOf(QLatin1Char('>'), i + 2);
            if (end < 0)
                return false;
            i = end;
            continue;
        }
        if (next == QLatin1Char('?')) {
            const int end = text.indexOf(QLatin1String("?>"), i + 2);
            if (end < 0)
                return false;
            i = end + 1;
            continue;
        }
        if (next == QLatin1Char('/')) {
            if (depth > 0)
                --depth;
            continue;
        }

        ++depth;
        if (depth > limit)
            return true;
        bool slash = false;
        bool inQuote = false;
        QChar quote;
        int end = i + 1;
        for (; end < text.size(); ++end) {
            const QChar current = text.at(end);
            if (inQuote) {
                if (current == quote)
                    inQuote = false;
                continue;
            }
            if (current == QLatin1Char('"') || current == QLatin1Char('\'')) {
                inQuote = true;
                quote = current;
                continue;
            }
            if (current == QLatin1Char('/')) {
                slash = true;
                continue;
            }
            if (current.isSpace())
                continue;
            if (current == QLatin1Char('>')) {
                if (slash && depth > 0)
                    --depth;
                break;
            }
            slash = false;
        }
        if (end >= text.size())
            return false;
        i = end;
    }
    return false;
}

QString explainXmlParseError(const QString &raw)
{
    const QString lower = raw.trimmed().toLower();
    if (lower.contains(QLatin1String("unexpected end")) || lower.contains(QLatin1String("premature end")))
        return QStringLiteral("XML 在写完之前就结束了。请检查是否少了结束标签，例如 </配置>。");
    if (lower.contains(QLatin1String("tag mismatch")))
        return QStringLiteral("XML 的开始标签和结束标签对不上。请检查两边的名称是否一致。");
    if (lower.contains(QLatin1String("start tag expected")))
        return QStringLiteral("这里应该是 XML 标签，例如 <配置>。");
    if (lower.contains(QLatin1String("extra content")))
        return QStringLiteral("XML 根元素结束之后还有多余内容。一份文件只能有一个根元素。");
    if (lower.contains(QLatin1String("invalid xml character")) || lower.contains(QLatin1String("invalid character")))
        return QStringLiteral("这里有 XML 不允许的字符。请删掉控制字符或不可见字符。");
    if (lower.contains(QLatin1String("parsing comment")))
        return QStringLiteral("XML 注释没有写完整。注释从 <!-- 开始，到 --> 结束，中间不要连续写两个减号。");
    if (lower.contains(QLatin1String("parsing element")))
        return QStringLiteral("XML 元素没有写完整。请检查尖括号、引号和结束标签。");
    if (lower.contains(QLatin1String("letter is expected")))
        return QStringLiteral("这里应该是字母。& 开头的符号需要写完整，例如 &amp;。");
    if (lower.contains(QLatin1String("unexpected character")))
        return QStringLiteral("这里有 XML 不能接受的写法。请检查引号是否成对，以及是不是写了多个根元素。");
    if (lower.contains(QLatin1String("expected")))
        return QStringLiteral("这里的 XML 符号不完整。请检查等号、引号和尖括号。");
    return QStringLiteral("XML 格式不正确。请检查标签是否成对、引号是否完整。");
}

ParamDocument parseXml(const QString &text)
{
    if (xmlNestingExceeds(text, kMaxParamDepth))
        return failed(ParamFormat::Xml, QStringLiteral("XML %1").arg(tooDeepMessage()));

    QDomDocument dom;
    QString error;
    int line = 0;
    int column = 0;
    if (!dom.setContent(text, false, &error, &line, &column)) {
        const QString where = (line > 0)
            ? QStringLiteral("第 %1 行第 %2 个字符：").arg(line).arg(qMax(column, 1))
            : QString();
        return failed(ParamFormat::Xml, where + explainXmlParseError(error), line, column);
    }

    XmlBuild build;
    build.limit = paramMaxNodes();
    ParamDocument document;
    document.ok = true;
    document.format = ParamFormat::Xml;
    document.root = std::make_unique<ParamNode>();
    document.root->kind = ParamKind::Document;
    build.nodes = 1;
    const QString doctype = captureDoctype(text);
    for (QDomNode child = dom.firstChild(); !child.isNull(); child = child.nextSibling()) {
        if (child.isProcessingInstruction()) {
            const QDomProcessingInstruction pi = child.toProcessingInstruction();
            if (pi.target() == QLatin1String("xml")) {
                document.xmlDeclaration = QStringLiteral("<?xml %1?>").arg(pi.data());
                continue;
            }
        }
        if (child.isDocumentType()) {
            auto node = makeXmlLeaf(ParamKind::Instruction, &build);
            if (!build.error.isEmpty())
                return failed(ParamFormat::Xml, build.error);
            if (!node)
                continue;
            node->name = QStringLiteral("!DOCTYPE");
            node->value = doctype.isEmpty()
                ? QStringLiteral("<!DOCTYPE ") + child.toDocumentType().name() + QLatin1Char('>')
                : doctype;
            document.root->append(std::move(node));
            continue;
        }
        auto converted = convertXmlNode(child, 1, &build);
        if (!build.error.isEmpty())
            return failed(ParamFormat::Xml, QStringLiteral("XML %1").arg(build.error));
        if (converted)
            document.root->append(std::move(converted));
    }
    if (countKind(document.root.get(), ParamKind::Element) != 1) {
        return failed(ParamFormat::Xml,
                      QStringLiteral("XML 需要有且只有一个根元素。请检查是否少了最外层标签，或写了两个并列的根元素。"));
    }
    return document;
}

} // namespace

bool paramJsonNumberIsValid(const QString &text)
{
    if (text.isEmpty())
        return false;
    JsonParser parser(text);
    const ParamDocument document = parser.parse();
    if (!document.ok || !document.root)
        return false;
    const ParamNode *node = document.root.get();
    if (node->kind == ParamKind::Document && node->childCount() == 1)
        node = node->children.front().get();
    return node->kind == ParamKind::Number && node->literal == text;
}

QString paramFormatName(ParamFormat format)
{
    switch (format) {
    case ParamFormat::Json:
        return QStringLiteral("JSON");
    case ParamFormat::Xml:
        return QStringLiteral("XML");
    case ParamFormat::Ini:
        return QStringLiteral("INI");
    case ParamFormat::Auto:
        return QStringLiteral("自动识别");
    case ParamFormat::Unsupported:
        break;
    }
    return QStringLiteral("未识别");
}

QString paramKindLabel(ParamKind kind)
{
    switch (kind) {
    case ParamKind::Document:
        return QStringLiteral("文档");
    case ParamKind::Object:
        return QStringLiteral("对象");
    case ParamKind::Array:
        return QStringLiteral("数组");
    case ParamKind::String:
        return QStringLiteral("字符串");
    case ParamKind::Number:
        return QStringLiteral("数字");
    case ParamKind::Bool:
        return QStringLiteral("布尔");
    case ParamKind::Null:
        return QStringLiteral("空");
    case ParamKind::Element:
        return QStringLiteral("元素");
    case ParamKind::Attribute:
        return QStringLiteral("属性");
    case ParamKind::Text:
        return QStringLiteral("文本");
    case ParamKind::Comment:
        return QStringLiteral("注释");
    case ParamKind::Section:
        return QStringLiteral("节");
    case ParamKind::Key:
        return QStringLiteral("键");
    case ParamKind::Instruction:
        return QStringLiteral("声明");
    }
    return QString();
}

QString paramNodeTitle(const ParamNode *node)
{
    if (!node)
        return {};
    switch (node->kind) {
    case ParamKind::Attribute:
        return QStringLiteral("@") + node->name;
    case ParamKind::Text:
        return QStringLiteral("#文本");
    case ParamKind::Comment:
        return QStringLiteral("#注释");
    case ParamKind::Instruction:
        return node->name == QLatin1String("!DOCTYPE") ? QStringLiteral("#文档类型")
                                                       : QStringLiteral("#") + node->name;
    default:
        return node->name;
    }
}

QString paramNodeValueText(const ParamNode *node)
{
    if (!node)
        return {};
    switch (node->kind) {
    case ParamKind::Object:
    case ParamKind::Array:
    case ParamKind::Section:
    case ParamKind::Document:
        return QStringLiteral("%1 项").arg(node->childCount());
    case ParamKind::Element: {
        if (countKind(node, ParamKind::Element) == 0 && countKind(node, ParamKind::Text) <= 1) {
            const ParamNode *text = onlyTextChild(node);
            return text ? text->value : QString();
        }
        return QStringLiteral("%1 项").arg(node->childCount());
    }
    case ParamKind::Null:
        return QStringLiteral("null");
    case ParamKind::Bool:
        return node->value == QLatin1String("true") ? QStringLiteral("true") : QStringLiteral("false");
    default:
        return node->value;
    }
}

bool paramNodeTitleEditable(const ParamNode *node)
{
    if (!node || !node->parent)
        return false;
    if (node->parent->kind == ParamKind::Array)
        return false;
    switch (node->kind) {
    case ParamKind::Attribute:
    case ParamKind::Element:
    case ParamKind::Section:
    case ParamKind::Key:
        return true;
    case ParamKind::String:
    case ParamKind::Number:
    case ParamKind::Bool:
    case ParamKind::Null:
        return node->parent->kind == ParamKind::Object;
    case ParamKind::Instruction:
        return node->name != QLatin1String("!DOCTYPE");
    default:
        return false;
    }
}

bool paramNodeValueEditable(const ParamNode *node)
{
    if (!node)
        return false;
    switch (node->kind) {
    case ParamKind::String:
    case ParamKind::Number:
    case ParamKind::Bool:
    case ParamKind::Attribute:
    case ParamKind::Text:
    case ParamKind::Comment:
    case ParamKind::Key:
    case ParamKind::Instruction:
        return true;
    case ParamKind::Element:
        return countKind(node, ParamKind::Element) == 0 && countKind(node, ParamKind::Text) <= 1;
    default:
        return false;
    }
}

ParamFormat detectParamFormat(const QString &text, const QString &suffix)
{
    const QString trimmed = text.trimmed();
    if (trimmed.startsWith(QLatin1Char('<')) || trimmed.startsWith(QLatin1String("<?xml"), Qt::CaseInsensitive))
        return ParamFormat::Xml;
    if (trimmed.startsWith(QLatin1Char('{')))
        return ParamFormat::Json;
    if (trimmed.startsWith(QLatin1Char('[')))
        return looksLikeIniSection(trimmed) ? ParamFormat::Ini : ParamFormat::Json;

    const QString ext = suffix.toLower();
    if (ext == QLatin1String("yaml") || ext == QLatin1String("yml"))
        return ParamFormat::Unsupported;
    if (ext == QLatin1String("xml") || ext == QLatin1String("xsd"))
        return ParamFormat::Xml;
    if (ext == QLatin1String("json"))
        return ParamFormat::Json;
    if (ext == QLatin1String("ini") || ext == QLatin1String("conf") || ext == QLatin1String("cfg")
        || ext == QLatin1String("properties"))
        return ParamFormat::Ini;
    if (looksLikeIni(text))
        return ParamFormat::Ini;
    return ParamFormat::Unsupported;
}

ParamDocument parseParamText(const QString &text, ParamFormat format, const QString &suffix)
{
    const int maxChars = paramMaxChars();
    if (text.size() > maxChars) {
        return failed(format == ParamFormat::Auto ? ParamFormat::Unsupported : format,
                      QStringLiteral("内容超过 %1 个字符，已停止解析，以免占满内存。").arg(maxChars));
    }
    if (text.trimmed().isEmpty()) {
        return failed(format == ParamFormat::Auto ? ParamFormat::Unsupported : format,
                      QStringLiteral("内容是空的，没有可解析的参数。"));
    }
    if (format == ParamFormat::Auto)
        format = detectParamFormat(text, suffix);
    switch (format) {
    case ParamFormat::Json:
        return JsonParser(text).parse();
    case ParamFormat::Xml:
        return parseXml(text);
    case ParamFormat::Ini:
        return parseIni(text);
    case ParamFormat::Unsupported: {
        const QString ext = suffix.toLower();
        if (ext == QLatin1String("yaml") || ext == QLatin1String("yml")) {
            return failed(ParamFormat::Unsupported,
                          QStringLiteral("暂不支持 YAML。目前可以检查 JSON、XML 和 INI。"));
        }
        return failed(ParamFormat::Unsupported,
                      QStringLiteral("无法识别格式。请在格式列表中选择 JSON、XML 或 INI。"));
    }
    case ParamFormat::Auto:
        break;
    }
    return failed(ParamFormat::Unsupported, QStringLiteral("无法识别格式。请在格式列表中选择 JSON、XML 或 INI。"));
}

QString serializeParam(const ParamDocument &document, QString *error)
{
    if (!document.root) {
        if (error)
            *error = QStringLiteral("没有可写回的结构。文件没有改动。");
        return {};
    }
    switch (document.format) {
    case ParamFormat::Json:
        return serializeJson(*document.root);
    case ParamFormat::Xml:
        return serializeXml(document, error);
    case ParamFormat::Ini:
        return serializeIni(*document.root);
    default:
        break;
    }
    if (error)
        *error = QStringLiteral("当前格式不能写回。文件没有改动。");
    return {};
}

} // namespace tw
