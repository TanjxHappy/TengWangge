#include "tools/param_check/param_document.h"

#include <QDomDocument>
#include <QDomProcessingInstruction>
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
    return QStringLiteral("第 %1 行，第 %2 列：").arg(line).arg(column) + message;
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
        auto value = parseValue();
        if (!value) {
            if (m_error.isEmpty())
                setError(QStringLiteral("解析失败"));
            return failed(ParamFormat::Json, m_error, m_errorLine, m_errorColumn);
        }
        skipWs();
        if (!eof()) {
            setError(QStringLiteral("末尾还有多余内容"));
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

    std::unique_ptr<ParamNode> parseValue()
    {
        if (m_depth >= 128) {
            setError(QStringLiteral("嵌套层数超过 128，已停止解析"));
            return nullptr;
        }
        DepthGuard guard(&m_depth);
        skipWs();
        if (eof()) {
            setError(QStringLiteral("这里缺少值"));
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
        setError(QStringLiteral("无法识别的值"));
        return nullptr;
    }

    std::unique_ptr<ParamNode> parseObject()
    {
        advance(); // {
        auto object = std::make_unique<ParamNode>();
        object->kind = ParamKind::Object;
        skipWs();
        if (ch() == QLatin1Char('}')) {
            advance();
            return object;
        }
        QStringList seen;
        while (!eof()) {
            skipWs();
            if (ch() != QLatin1Char('"')) {
                setError(QStringLiteral("对象的键必须是字符串"));
                return nullptr;
            }
            QString key;
            if (!readString(&key))
                return nullptr;
            if (seen.contains(key)) {
                setError(QStringLiteral("重复的键「") + key + QStringLiteral("」"));
                return nullptr;
            }
            seen.append(key);
            skipWs();
            if (ch() != QLatin1Char(':')) {
                setError(QStringLiteral("键后面缺少冒号"));
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
                    setError(QStringLiteral("末尾不能有多余的逗号"));
                    return nullptr;
                }
                continue;
            }
            if (ch() == QLatin1Char('}')) {
                advance();
                return object;
            }
            setError(QStringLiteral("对象里缺少逗号或右花括号"));
            return nullptr;
        }
        setError(QStringLiteral("对象没有结束"));
        return nullptr;
    }

    std::unique_ptr<ParamNode> parseArray()
    {
        advance(); // [
        auto array = std::make_unique<ParamNode>();
        array->kind = ParamKind::Array;
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
                    setError(QStringLiteral("末尾不能有多余的逗号"));
                    return nullptr;
                }
                continue;
            }
            if (ch() == QLatin1Char(']')) {
                advance();
                return array;
            }
            setError(QStringLiteral("数组里缺少逗号或右方括号"));
            return nullptr;
        }
        setError(QStringLiteral("数组没有结束"));
        return nullptr;
    }

    std::unique_ptr<ParamNode> parseStringValue()
    {
        QString text;
        if (!readString(&text))
            return nullptr;
        auto node = std::make_unique<ParamNode>();
        node->kind = ParamKind::String;
        node->value = text;
        return node;
    }

    bool readString(QString *out)
    {
        if (ch() != QLatin1Char('"')) {
            setError(QStringLiteral("这里应该是字符串"));
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
                    setError(QStringLiteral("字符串的转义不完整"));
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
                    setError(QStringLiteral("不支持的转义字符"));
                    return false;
                }
                continue;
            }
            if (c.unicode() < 0x20) {
                setError(QStringLiteral("字符串里有未转义的控制字符"));
                return false;
            }
            value += c;
            advance();
        }
        setError(QStringLiteral("字符串没有结束"));
        return false;
    }

    bool readUnicodeEscape(QString *value)
    {
        int code = 0;
        if (!readHex4(&code))
            return false;
        if (code >= 0xDC00 && code <= 0xDFFF) {
            setError(QStringLiteral("Unicode 转义的低代理项缺少高代理项"));
            return false;
        }
        if (code >= 0xD800 && code <= 0xDBFF) {
            if (ch() != QLatin1Char('\\')) {
                setError(QStringLiteral("Unicode 转义的代理项不完整"));
                return false;
            }
            advance();
            if (ch() != QLatin1Char('u')) {
                setError(QStringLiteral("Unicode 转义的代理项不完整"));
                return false;
            }
            advance();
            int low = 0;
            if (!readHex4(&low))
                return false;
            if (low < 0xDC00 || low > 0xDFFF) {
                setError(QStringLiteral("Unicode 转义的代理项不完整"));
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
                setError(QStringLiteral("Unicode 转义需要 4 位十六进制"));
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
                setError(QStringLiteral("Unicode 转义需要 4 位十六进制"));
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
                setError(QStringLiteral("无法识别的值"));
                return nullptr;
            }
            advance();
        }
        if (!eof() && (ch().isLetterOrNumber() || ch() == QLatin1Char('_'))) {
            setError(QStringLiteral("无法识别的值"));
            return nullptr;
        }
        auto node = std::make_unique<ParamNode>();
        node->kind = kind;
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
            setError(QStringLiteral("数字不完整"));
            return nullptr;
        }
        if (ch() == QLatin1Char('0')) {
            advance();
            if (!eof() && ch().isDigit()) {
                setError(QStringLiteral("数字不能有前导零"));
                return nullptr;
            }
        } else {
            while (!eof() && ch().isDigit())
                advance();
        }
        if (ch() == QLatin1Char('.')) {
            advance();
            if (eof() || !ch().isDigit()) {
                setError(QStringLiteral("小数点后面缺少数字"));
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
                setError(QStringLiteral("指数后面缺少数字"));
                return nullptr;
            }
            while (!eof() && ch().isDigit())
                advance();
        }
        auto node = std::make_unique<ParamNode>();
        node->kind = ParamKind::Number;
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

std::unique_ptr<ParamNode> convertXmlNode(const QDomNode &domNode)
{
    if (domNode.isElement()) {
        const QDomElement element = domNode.toElement();
        auto node = std::make_unique<ParamNode>();
        node->kind = ParamKind::Element;
        node->name = element.tagName();
        const QDomNamedNodeMap attributes = element.attributes();
        for (int i = 0; i < attributes.count(); ++i) {
            const QDomAttr attr = attributes.item(i).toAttr();
            auto child = std::make_unique<ParamNode>();
            child->kind = ParamKind::Attribute;
            child->name = attr.name();
            child->value = attr.value();
            node->append(std::move(child));
        }
        for (QDomNode child = element.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if (child.isText() && !child.isCDATASection() && child.nodeValue().trimmed().isEmpty())
                continue;
            auto converted = convertXmlNode(child);
            if (converted)
                node->append(std::move(converted));
        }
        return node;
    }
    if (domNode.isCDATASection()) {
        auto node = std::make_unique<ParamNode>();
        node->kind = ParamKind::Text;
        node->value = domNode.nodeValue();
        node->cdata = true;
        return node;
    }
    if (domNode.isText()) {
        if (domNode.nodeValue().trimmed().isEmpty())
            return nullptr;
        auto node = std::make_unique<ParamNode>();
        node->kind = ParamKind::Text;
        node->value = domNode.nodeValue();
        return node;
    }
    if (domNode.isComment()) {
        auto node = std::make_unique<ParamNode>();
        node->kind = ParamKind::Comment;
        node->value = domNode.nodeValue();
        return node;
    }
    if (domNode.isProcessingInstruction()) {
        const QDomProcessingInstruction pi = domNode.toProcessingInstruction();
        auto node = std::make_unique<ParamNode>();
        node->kind = ParamKind::Instruction;
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
                *error = QStringLiteral("元素名无效：") + node.name;
            return false;
        }
        QDomElement element = dom->createElement(node.name);
        parent.appendChild(element);
        for (const auto &child : node.children) {
            if (child->kind != ParamKind::Attribute)
                continue;
            if (!xmlNameOk(child->name)) {
                if (error)
                    *error = QStringLiteral("属性名无效：") + child->name;
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
                *error = QStringLiteral("XML 注释里不能包含连续的连字符");
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
            *error = QStringLiteral("XML 树里有不能写回的节点");
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

QString unescapeIniQuoted(const QString &text)
{
    QString out;
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\\') && i + 1 < text.size()) {
            const QChar next = text.at(i + 1);
            if (next == QLatin1Char('\\') || next == QLatin1Char('"')) {
                out += next;
                ++i;
                continue;
            }
        }
        out += text.at(i);
    }
    return out;
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
        *out += QLatin1Char('\n');
    }
}

QString serializeIni(const ParamNode &root)
{
    QString out;
    for (const auto &child : root.children) {
        if (child->kind == ParamKind::Section) {
            out += QLatin1Char('[') + child->name + QLatin1String("]\n");
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

ParamDocument parseIni(const QString &text)
{
    ParamDocument document;
    document.ok = true;
    document.format = ParamFormat::Ini;
    document.root = std::make_unique<ParamNode>();
    document.root->kind = ParamKind::Document;
    ParamNode *section = document.root.get();

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        const QString trimmed = line.trimmed();
        const int lineNo = i + 1;
        if (trimmed.isEmpty())
            continue;
        if (trimmed.startsWith(QLatin1Char(';')) || trimmed.startsWith(QLatin1Char('#'))) {
            auto comment = std::make_unique<ParamNode>();
            comment->kind = ParamKind::Comment;
            comment->literal = trimmed.left(1);
            comment->value = trimmed.mid(1).trimmed();
            section->append(std::move(comment));
            continue;
        }
        if (trimmed.startsWith(QLatin1Char('['))) {
            if (!trimmed.endsWith(QLatin1Char(']')) || trimmed.size() < 3) {
                return failed(ParamFormat::Ini,
                              QStringLiteral("第 %1 行：节名需要用方括号包起来").arg(lineNo), lineNo, 1);
            }
            const QString name = trimmed.mid(1, trimmed.size() - 2).trimmed();
            if (name.isEmpty() || name.contains(QLatin1Char('[')) || name.contains(QLatin1Char(']'))) {
                return failed(ParamFormat::Ini, QStringLiteral("第 %1 行：节名无效").arg(lineNo), lineNo, 1);
            }
            auto created = std::make_unique<ParamNode>();
            created->kind = ParamKind::Section;
            created->name = name;
            section = document.root->append(std::move(created));
            continue;
        }
        const int eq = trimmed.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            return failed(ParamFormat::Ini,
                          QStringLiteral("第 %1 行：这一行不是「键=值」，也不是节或注释").arg(lineNo),
                          lineNo, 1);
        }
        const QString key = trimmed.left(eq).trimmed();
        QString value = trimmed.mid(eq + 1).trimmed();
        if (key.isEmpty() || key.contains(QLatin1Char(' ')) || key.contains(QLatin1Char('\t'))) {
            return failed(ParamFormat::Ini, QStringLiteral("第 %1 行：键名不能为空，也不能包含空白").arg(lineNo),
                          lineNo, 1);
        }
        bool quoted = false;
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) {
            quoted = true;
            value = unescapeIniQuoted(value.mid(1, value.size() - 2));
        }
        auto entry = std::make_unique<ParamNode>();
        entry->kind = ParamKind::Key;
        entry->name = key;
        entry->value = value;
        entry->quoted = quoted;
        section->append(std::move(entry));
    }
    return document;
}

ParamDocument parseXml(const QString &text)
{
    QDomDocument dom;
    QString error;
    int line = 0;
    int column = 0;
    if (!dom.setContent(text, false, &error, &line, &column)) {
        return failed(ParamFormat::Xml,
                      QStringLiteral("XML 解析失败（第 %1 行，第 %2 列）：%3").arg(line).arg(column).arg(error),
                      line, column);
    }

    ParamDocument document;
    document.ok = true;
    document.format = ParamFormat::Xml;
    document.root = std::make_unique<ParamNode>();
    document.root->kind = ParamKind::Document;
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
            auto node = std::make_unique<ParamNode>();
            node->kind = ParamKind::Instruction;
            node->name = QStringLiteral("!DOCTYPE");
            node->value = doctype.isEmpty()
                ? QStringLiteral("<!DOCTYPE ") + child.toDocumentType().name() + QLatin1Char('>')
                : doctype;
            document.root->append(std::move(node));
            continue;
        }
        auto converted = convertXmlNode(child);
        if (converted)
            document.root->append(std::move(converted));
    }
    if (countKind(document.root.get(), ParamKind::Element) != 1) {
        return failed(ParamFormat::Xml, QStringLiteral("XML 需要恰好有一个根元素"));
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
            *error = QStringLiteral("没有可写回的结构");
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
        *error = QStringLiteral("当前格式不能写回");
    return {};
}

} // namespace tw
