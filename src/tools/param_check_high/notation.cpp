#include "tools/param_check_high/notation.h"

#include <QXmlStreamReader>

namespace tw {
namespace {

constexpr int kMaxDepth = 64;
constexpr int kMaxNodes = 20000;
constexpr int kMaxChars = 1500000;

bool isAsciiDigit(QChar character)
{
    return character >= QLatin1Char('0') && character <= QLatin1Char('9');
}

QString escapeJson(const QString &text)
{
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar character : text) {
        switch (character.unicode()) {
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
            if (character.unicode() < 0x20) {
                out += QStringLiteral("\\u%1")
                           .arg(character.unicode(), 4, 16, QLatin1Char('0'));
            } else {
                out += character;
            }
            break;
        }
    }
    return out;
}

QString escapeXml(const QString &text, bool attribute)
{
    QString out;
    out.reserve(text.size());
    for (const QChar character : text) {
        if (character == QLatin1Char('&'))
            out += QLatin1String("&amp;");
        else if (character == QLatin1Char('<'))
            out += QLatin1String("&lt;");
        else if (character == QLatin1Char('>'))
            out += QLatin1String("&gt;");
        else if (attribute && character == QLatin1Char('"'))
            out += QLatin1String("&quot;");
        else if (character == QLatin1Char('\t') || character == QLatin1Char('\n')
                 || character == QLatin1Char('\r') || character.unicode() >= 0x20)
            out += character;
        else
            out += QStringLiteral("&#x%1;").arg(character.unicode(), 0, 16);
    }
    return out;
}

class JsonReader
{
public:
    explicit JsonReader(const QString &text)
        : m_text(text)
    {
    }

    std::unique_ptr<Node> readRoot()
    {
        skipSpace();
        if (m_failed)
            return nullptr;
        if (m_index >= m_text.size())
            return failNode(QStringLiteral("还没有内容。请输入一个 JSON 值。"));
        std::unique_ptr<Node> value = readValue(1);
        if (!value)
            return nullptr;
        skipSpace();
        if (m_failed)
            return nullptr;
        if (m_index < m_text.size())
            return failNode(QStringLiteral("后面还有多余内容。一份 JSON 只能有一个值。"));
        return value;
    }

    QString message() const { return m_message; }
    int line() const { return m_lineOut; }
    int column() const { return m_columnOut; }

private:
    bool fail(const QString &why)
    {
        if (!m_failed) {
            m_failed = true;
            m_lineOut = m_line;
            m_columnOut = m_column;
            m_message = QStringLiteral("第 %1 行第 %2 列：%3").arg(m_line).arg(m_column).arg(why);
        }
        return false;
    }

    std::unique_ptr<Node> failNode(const QString &why)
    {
        fail(why);
        return nullptr;
    }

    QChar peek() const
    {
        if (m_index >= m_text.size())
            return {};
        return m_text.at(m_index);
    }

    QChar next()
    {
        if (m_index >= m_text.size())
            return {};
        const QChar character = m_text.at(m_index++);
        if (character == QLatin1Char('\n')) {
            ++m_line;
            m_column = 1;
        } else if (character == QLatin1Char('\r')) {
            if (m_index < m_text.size() && m_text.at(m_index) == QLatin1Char('\n'))
                ++m_index;
            ++m_line;
            m_column = 1;
        } else {
            ++m_column;
        }
        return character;
    }

    void skipSpace()
    {
        while (peek() == QLatin1Char(' ') || peek() == QLatin1Char('\t') || peek() == QLatin1Char('\n')
               || peek() == QLatin1Char('\r'))
            next();
    }

    std::unique_ptr<Node> make(NodeKind kind)
    {
        if (m_failed)
            return nullptr;
        if (++m_nodes > kMaxNodes) {
            fail(QStringLiteral("条目太多（超过 2 万）。请把文件拆小后再检查，以免窗口卡住。"));
            return nullptr;
        }
        auto node = std::make_unique<Node>();
        node->kind = kind;
        return node;
    }

    bool readHex4(uint *code)
    {
        uint value = 0;
        for (int i = 0; i < 4; ++i) {
            if (m_index >= m_text.size())
                return fail(QStringLiteral("\\u 后面需要 4 位十六进制数字。"));
            const QChar character = next();
            int nibble = -1;
            if (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
                nibble = character.unicode() - u'0';
            else if (character >= QLatin1Char('a') && character <= QLatin1Char('f'))
                nibble = character.unicode() - u'a' + 10;
            else if (character >= QLatin1Char('A') && character <= QLatin1Char('F'))
                nibble = character.unicode() - u'A' + 10;
            if (nibble < 0)
                return fail(QStringLiteral("\\u 后面需要 4 位十六进制数字。"));
            value = (value << 4) | static_cast<uint>(nibble);
        }
        *code = value;
        return true;
    }

    bool readString(QString *out)
    {
        if (peek() != QLatin1Char('"'))
            return fail(QStringLiteral("这里应该是用双引号括起来的文字。"));
        next();
        QString value;
        while (m_index < m_text.size()) {
            const QChar character = next();
            if (character == QLatin1Char('"')) {
                *out = value;
                return true;
            }
            if (character == QLatin1Char('\\')) {
                if (m_index >= m_text.size())
                    return fail(QStringLiteral("转义字符还没写完。"));
                const QChar escaped = next();
                switch (escaped.unicode()) {
                case '"':
                case '\\':
                case '/':
                    value += escaped;
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
                case 'u': {
                    uint code = 0;
                    if (!readHex4(&code))
                        return false;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (!(peek() == QLatin1Char('\\') && m_index + 1 < m_text.size()
                              && m_text.at(m_index + 1) == QLatin1Char('u')))
                            return fail(QStringLiteral("这里有一个不完整的汉字或表情字符。"));
                        next();
                        next();
                        uint low = 0;
                        if (!readHex4(&low))
                            return false;
                        if (low < 0xDC00 || low > 0xDFFF)
                            return fail(QStringLiteral("这里有一个不完整的汉字或表情字符。"));
                        const uint unit = 0x10000 + (((code - 0xD800) << 10) | (low - 0xDC00));
                        value += QString::fromUcs4(&unit, 1);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return fail(QStringLiteral("这里有一个不完整的汉字或表情字符。"));
                    } else {
                        value += QChar(static_cast<ushort>(code));
                    }
                    break;
                }
                default:
                    return fail(QStringLiteral("不认识这个转义字符。"));
                }
            } else if (character.unicode() < 0x20) {
                return fail(QStringLiteral("引号里面不能直接换行，请写成 \\n。"));
            } else {
                value += character;
            }
        }
        return fail(QStringLiteral("字符串的结束引号还没出现。"));
    }

    bool readNumber(QString *spelling)
    {
        const int start = m_index;
        if (peek() == QLatin1Char('-'))
            next();
        if (!isAsciiDigit(peek()))
            return fail(QStringLiteral("这里的数字不完整。"));
        if (peek() == QLatin1Char('0')) {
            next();
            if (isAsciiDigit(peek()))
                return fail(QStringLiteral("数字不能有多余的前导 0。"));
        } else {
            while (isAsciiDigit(peek()))
                next();
        }
        if (peek() == QLatin1Char('.')) {
            next();
            if (!isAsciiDigit(peek()))
                return fail(QStringLiteral("小数点后面还要有数字。"));
            while (isAsciiDigit(peek()))
                next();
        }
        if (peek() == QLatin1Char('e') || peek() == QLatin1Char('E')) {
            next();
            if (peek() == QLatin1Char('+') || peek() == QLatin1Char('-'))
                next();
            if (!isAsciiDigit(peek()))
                return fail(QStringLiteral("科学计数法后面还要有数字。"));
            while (isAsciiDigit(peek()))
                next();
        }
        *spelling = m_text.mid(start, m_index - start);
        return true;
    }

    bool matchWord(const char *word)
    {
        const int startIndex = m_index;
        const int startLine = m_line;
        const int startColumn = m_column;
        for (const char *cursor = word; *cursor != '\0'; ++cursor) {
            if (peek() != QLatin1Char(*cursor)) {
                m_index = startIndex;
                m_line = startLine;
                m_column = startColumn;
                return false;
            }
            next();
        }
        const QChar after = peek();
        if (after.isLetterOrNumber() || after == QLatin1Char('_')) {
            m_index = startIndex;
            m_line = startLine;
            m_column = startColumn;
            return false;
        }
        return true;
    }

    std::unique_ptr<Node> readValue(int depth)
    {
        if (m_failed)
            return nullptr;
        if (depth > kMaxDepth)
            return failNode(QStringLiteral("嵌套超过 64 层，继续展开会把界面撑住。请把过深的层级拆开后再检查。"));
        const int line = m_line;
        const int column = qMax(1, m_column);
        const QChar character = peek();
        if (character == QLatin1Char('{'))
            return readObject(depth);
        if (character == QLatin1Char('['))
            return readArray(depth);
        if (character == QLatin1Char('"')) {
            QString text;
            if (!readString(&text))
                return nullptr;
            std::unique_ptr<Node> node = make(NodeKind::Phrase);
            if (!node)
                return nullptr;
            node->value = text;
            node->line = line;
            node->column = column;
            return node;
        }
        if (character == QLatin1Char('t') || character == QLatin1Char('f') || character == QLatin1Char('n')) {
            if (matchWord("true")) {
                std::unique_ptr<Node> node = make(NodeKind::Switch);
                if (!node)
                    return nullptr;
                node->value = QStringLiteral("true");
                node->line = line;
                node->column = column;
                return node;
            }
            if (matchWord("false")) {
                std::unique_ptr<Node> node = make(NodeKind::Switch);
                if (!node)
                    return nullptr;
                node->value = QStringLiteral("false");
                node->line = line;
                node->column = column;
                return node;
            }
            if (matchWord("null")) {
                std::unique_ptr<Node> node = make(NodeKind::Vacant);
                if (!node)
                    return nullptr;
                node->line = line;
                node->column = column;
                return node;
            }
            return failNode(QStringLiteral("这里应该是 true、false 或 null。"));
        }
        if (character == QLatin1Char('-') || isAsciiDigit(character)) {
            QString spelling;
            if (!readNumber(&spelling))
                return nullptr;
            std::unique_ptr<Node> node = make(NodeKind::Quantity);
            if (!node)
                return nullptr;
            node->spelling = spelling;
            node->value = spelling;
            node->line = line;
            node->column = column;
            return node;
        }
        if (character.isNull())
            return failNode(QStringLiteral("JSON 还没写完。请补上缺少的引号、括号或内容。"));
        return failNode(QStringLiteral(
            "这里有一个无法识别的内容。JSON 的值只能是对象、数组、字符串、数字、true、false 或 null。"));
    }

    std::unique_ptr<Node> readObject(int depth)
    {
        const int line = m_line;
        const int column = qMax(1, m_column);
        next();
        std::unique_ptr<Node> object = make(NodeKind::Map);
        if (!object)
            return nullptr;
        object->line = line;
        object->column = column;
        skipSpace();
        if (peek() == QLatin1Char('}')) {
            next();
            return object;
        }
        QStringList seen;
        while (!m_failed) {
            skipSpace();
            if (peek() != QLatin1Char('"'))
                return failNode(QStringLiteral("对象的键需要用双引号括起来。"));
            const int keyLine = m_line;
            const int keyColumn = qMax(1, m_column);
            QString key;
            if (!readString(&key))
                return nullptr;
            if (seen.contains(key)) {
                QString why = QStringLiteral("对象里有重复的键「");
                why += key.left(40);
                why += QStringLiteral("」。同一个名字只能出现一次。");
                return failNode(why);
            }
            seen.append(key);
            skipSpace();
            if (peek() != QLatin1Char(':'))
                return failNode(QStringLiteral("键和值之间需要一个冒号。"));
            next();
            skipSpace();
            std::unique_ptr<Node> child = readValue(depth + 1);
            if (!child)
                return nullptr;
            child->name = key;
            child->line = keyLine;
            child->column = keyColumn;
            object->append(std::move(child));
            skipSpace();
            if (peek() == QLatin1Char(',')) {
                next();
                skipSpace();
                if (peek() == QLatin1Char('}'))
                    return failNode(QStringLiteral("这里多写了一个逗号。请删掉末尾的逗号。"));
                continue;
            }
            if (peek() == QLatin1Char('}')) {
                next();
                return object;
            }
            if (peek().isNull())
                return failNode(QStringLiteral("JSON 还没写完。请补上缺少的引号、括号或内容。"));
            return failNode(QStringLiteral("键值后面应该是逗号或者右花括号 }。"));
        }
        return nullptr;
    }

    std::unique_ptr<Node> readArray(int depth)
    {
        const int line = m_line;
        const int column = qMax(1, m_column);
        next();
        std::unique_ptr<Node> array = make(NodeKind::List);
        if (!array)
            return nullptr;
        array->line = line;
        array->column = column;
        skipSpace();
        if (peek() == QLatin1Char(']')) {
            next();
            return array;
        }
        while (!m_failed) {
            skipSpace();
            std::unique_ptr<Node> child = readValue(depth + 1);
            if (!child)
                return nullptr;
            array->append(std::move(child));
            skipSpace();
            if (peek() == QLatin1Char(',')) {
                next();
                skipSpace();
                if (peek() == QLatin1Char(']'))
                    return failNode(QStringLiteral("这里多写了一个逗号。请删掉末尾的逗号。"));
                continue;
            }
            if (peek() == QLatin1Char(']')) {
                next();
                return array;
            }
            if (peek().isNull())
                return failNode(QStringLiteral("JSON 还没写完。请补上缺少的引号、括号或内容。"));
            return failNode(QStringLiteral("数组元素后面应该是逗号或者右方括号 ]。"));
        }
        return nullptr;
    }

    const QString &m_text;
    int m_index = 0;
    int m_line = 1;
    int m_column = 1;
    int m_nodes = 0;
    bool m_failed = false;
    QString m_message;
    int m_lineOut = 1;
    int m_columnOut = 1;
};

bool isIniSectionLine(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.size() < 3 || !trimmed.startsWith(QLatin1Char('[')) || !trimmed.endsWith(QLatin1Char(']')))
        return false;
    if (trimmed.indexOf(QLatin1Char(']')) != trimmed.size() - 1)
        return false;
    const QString inner = trimmed.mid(1, trimmed.size() - 2).trimmed();
    if (inner.isEmpty() || inner.contains(QLatin1Char(',')) || inner.contains(QLatin1Char('"'))
        || inner.contains(QLatin1Char('\'')))
        return false;
    const QChar first = inner.at(0);
    if (first.isDigit() || first == QLatin1Char('-') || first == QLatin1Char('+') || first == QLatin1Char('{')
        || first == QLatin1Char('[') || first == QLatin1Char('"'))
        return false;
    if (inner == QLatin1String("true") || inner == QLatin1String("false") || inner == QLatin1String("null"))
        return false;
    return true;
}

bool looksLikeYamlLine(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')) || trimmed.contains(QLatin1Char('=')))
        return false;
    const int colon = trimmed.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return false;
    const QChar first = trimmed.at(0);
    return first.isLetter() || first == QLatin1Char('_') || first.unicode() > 127;
}

QString yamlMessage()
{
    return QStringLiteral("这是 YAML 文件。参数检查high 只认识 JSON、XML 和 INI，请先转换成其中一种再打开。");
}

QString unknownMessage()
{
    return QStringLiteral("看不出这是 JSON、XML 还是 INI。请在上方选择格式，或给文件换上 .json、.xml、.ini 后缀。");
}

bool resolveNotation(const QString &text, Notation requested, const QString &suffix, Notation *resolved,
                     QString *message)
{
    const QString ext = suffix.trimmed().toLower();
    if (requested == Notation::Json || requested == Notation::Xml || requested == Notation::Ini) {
        *resolved = requested;
        return true;
    }
    if (ext == QLatin1String("yml") || ext == QLatin1String("yaml")) {
        *message = yamlMessage();
        return false;
    }

    int start = 0;
    while (start < text.size()) {
        const QChar character = text.at(start);
        if (character != QLatin1Char(' ') && character != QLatin1Char('\t') && character != QLatin1Char('\n')
            && character != QLatin1Char('\r'))
            break;
        ++start;
    }
    if (start >= text.size()) {
        *message = QStringLiteral("还没有内容。请输入 JSON、XML 或 INI。");
        return false;
    }

    const QChar first = text.at(start);
    if (first == QLatin1Char('<')) {
        *resolved = Notation::Xml;
        return true;
    }
    if (first == QLatin1Char('{')) {
        *resolved = Notation::Json;
        return true;
    }
    if (first == QLatin1Char('[')) {
        QString firstLine = text.mid(start);
        const int newline = firstLine.indexOf(QLatin1Char('\n'));
        if (newline >= 0)
            firstLine = firstLine.left(newline);
        if (firstLine.endsWith(QLatin1Char('\r')))
            firstLine.chop(1);
        *resolved = isIniSectionLine(firstLine) ? Notation::Ini : Notation::Json;
        return true;
    }
    if (ext == QLatin1String("xml")) {
        *resolved = Notation::Xml;
        return true;
    }
    if (ext == QLatin1String("json")) {
        *resolved = Notation::Json;
        return true;
    }
    if (ext == QLatin1String("ini") || ext == QLatin1String("cfg") || ext == QLatin1String("conf")) {
        *resolved = Notation::Ini;
        return true;
    }

    const QStringList lines = text.split(QLatin1Char('\n'));
    bool yaml = false;
    for (const QString &line : lines) {
        if (isIniSectionLine(line) || line.contains(QLatin1Char('='))) {
            *resolved = Notation::Ini;
            return true;
        }
        if (looksLikeYamlLine(line))
            yaml = true;
    }
    if (yaml) {
        *message = yamlMessage();
        return false;
    }
    *message = unknownMessage();
    return false;
}

Sheet parseJson(const QString &text)
{
    Sheet sheet;
    sheet.notation = Notation::Json;
    JsonReader reader(text);
    sheet.root = reader.readRoot();
    if (!sheet.root) {
        sheet.message = reader.message().isEmpty()
                            ? QStringLiteral("第 1 行：这份 JSON 没能读懂。请检查括号、引号和逗号。")
                            : reader.message();
        sheet.line = qMax(1, reader.line());
        sheet.column = qMax(1, reader.column());
        return sheet;
    }
    sheet.ok = true;
    return sheet;
}

QString explainXml(const QXmlStreamReader &reader)
{
    const int line = qMax(1, static_cast<int>(reader.lineNumber()));
    const int column = qMax(1, static_cast<int>(reader.columnNumber()));
    QString detail;
    switch (reader.error()) {
    case QXmlStreamReader::PrematureEndOfDocumentError:
        detail = QStringLiteral("文件已经结束，但还有标签没有闭合。请补上结束标签。");
        break;
    case QXmlStreamReader::NotWellFormedError:
        detail = QStringLiteral("这里的标签或符号不完整。请检查尖括号和引号是不是成对。");
        break;
    case QXmlStreamReader::UnexpectedElementError:
        detail = QStringLiteral("这里出现了不该出现的内容。");
        break;
    default:
        detail = QStringLiteral("这里的 XML 无法识别。请检查标签是不是写错了。");
        break;
    }
    return QStringLiteral("第 %1 行第 %2 列：XML %3").arg(line).arg(column).arg(detail);
}

Sheet parseXml(const QString &text)
{
    Sheet sheet;
    sheet.notation = Notation::Xml;
    const int declaration = text.indexOf(QLatin1String("<?xml"));
    if (declaration >= 0) {
        const int end = text.indexOf(QLatin1String("?>"), declaration);
        if (end > declaration)
            sheet.prologue = text.mid(declaration, end + 2 - declaration);
    }

    auto root = std::make_unique<Node>();
    root->kind = NodeKind::Document;
    root->line = 1;
    root->column = 1;
    QXmlStreamReader reader(text);
    std::vector<Node *> stack;
    stack.push_back(root.get());
    int depth = 0;
    int nodes = 1;

    auto tooDeep = [&]() {
        sheet.ok = false;
        sheet.line = qMax(1, static_cast<int>(reader.lineNumber()));
        sheet.column = qMax(1, static_cast<int>(reader.columnNumber()));
        sheet.message = QStringLiteral("第 %1 行：嵌套超过 64 层，继续展开会把界面撑住。请把过深的层级拆开后再检查。")
                            .arg(sheet.line);
    };
    auto tooMany = [&]() {
        sheet.ok = false;
        sheet.line = qMax(1, static_cast<int>(reader.lineNumber()));
        sheet.column = qMax(1, static_cast<int>(reader.columnNumber()));
        sheet.message = QStringLiteral("第 %1 行：条目太多（超过 2 万）。请把文件拆小后再检查，以免窗口卡住。")
                            .arg(sheet.line);
    };

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.hasError())
            break;
        switch (reader.tokenType()) {
        case QXmlStreamReader::StartElement: {
            ++depth;
            if (depth > kMaxDepth) {
                tooDeep();
                return sheet;
            }
            if (++nodes > kMaxNodes) {
                tooMany();
                return sheet;
            }
            auto element = std::make_unique<Node>();
            element->kind = NodeKind::Element;
            element->name = reader.name().toString();
            element->line = qMax(1, static_cast<int>(reader.lineNumber()));
            element->column = qMax(1, static_cast<int>(reader.columnNumber()));
            Node *raw = stack.back()->append(std::move(element));
            const auto attributes = reader.attributes();
            for (const QXmlStreamAttribute &attribute : attributes) {
                if (++nodes > kMaxNodes) {
                    tooMany();
                    return sheet;
                }
                auto attr = std::make_unique<Node>();
                attr->kind = NodeKind::Attribute;
                attr->name = attribute.qualifiedName().toString();
                attr->value = attribute.value().toString();
                attr->line = raw->line;
                attr->column = raw->column;
                raw->append(std::move(attr));
            }
            stack.push_back(raw);
            break;
        }
        case QXmlStreamReader::EndElement:
            if (stack.size() > 1)
                stack.pop_back();
            --depth;
            break;
        case QXmlStreamReader::Characters: {
            if (!reader.isCDATA() && reader.isWhitespace())
                break;
            if (++nodes > kMaxNodes) {
                tooMany();
                return sheet;
            }
            Node *parent = stack.back();
            if (!parent->children.empty() && parent->children.back()->kind == NodeKind::Characters
                && parent->children.back()->cdata == reader.isCDATA()) {
                parent->children.back()->value += reader.text().toString();
                break;
            }
            auto textNode = std::make_unique<Node>();
            textNode->kind = NodeKind::Characters;
            textNode->cdata = reader.isCDATA();
            textNode->value = reader.text().toString();
            textNode->line = qMax(1, static_cast<int>(reader.lineNumber()));
            textNode->column = qMax(1, static_cast<int>(reader.columnNumber()));
            parent->append(std::move(textNode));
            break;
        }
        case QXmlStreamReader::Comment: {
            if (++nodes > kMaxNodes) {
                tooMany();
                return sheet;
            }
            auto comment = std::make_unique<Node>();
            comment->kind = NodeKind::Comment;
            comment->value = reader.text().toString();
            comment->line = qMax(1, static_cast<int>(reader.lineNumber()));
            comment->column = qMax(1, static_cast<int>(reader.columnNumber()));
            stack.back()->append(std::move(comment));
            break;
        }
        case QXmlStreamReader::ProcessingInstruction: {
            if (++nodes > kMaxNodes) {
                tooMany();
                return sheet;
            }
            auto instruction = std::make_unique<Node>();
            instruction->kind = NodeKind::Comment;
            instruction->spelling = QStringLiteral("pi");
            instruction->name = reader.processingInstructionTarget().toString();
            instruction->value = reader.processingInstructionData().toString();
            instruction->line = qMax(1, static_cast<int>(reader.lineNumber()));
            instruction->column = qMax(1, static_cast<int>(reader.columnNumber()));
            stack.back()->append(std::move(instruction));
            break;
        }
        case QXmlStreamReader::DTD:
            sheet.line = qMax(1, static_cast<int>(reader.lineNumber()));
            sheet.column = qMax(1, static_cast<int>(reader.columnNumber()));
            sheet.message = QStringLiteral("第 %1 行：XML 不支持 DTD。请只保留元素、属性和文本。").arg(sheet.line);
            return sheet;
        default:
            break;
        }
    }

    if (reader.hasError()) {
        sheet.message = explainXml(reader);
        sheet.line = qMax(1, static_cast<int>(reader.lineNumber()));
        sheet.column = qMax(1, static_cast<int>(reader.columnNumber()));
        return sheet;
    }

    int roots = 0;
    for (int i = 0; i < root->childCount(); ++i) {
        if (root->children[static_cast<size_t>(i)]->kind == NodeKind::Element)
            ++roots;
    }
    if (roots != 1) {
        sheet.line = 1;
        sheet.column = 1;
        sheet.message = roots == 0
                            ? QStringLiteral("第 1 行：XML 里没有根节点。请用一对标签把内容包起来。")
                            : QStringLiteral("第 1 行：XML 只能有一个根节点。请把多余的内容包进同一个根节点。");
        return sheet;
    }
    sheet.root = std::move(root);
    sheet.ok = true;
    return sheet;
}

QString unescapeIni(const QString &inner, bool *ok)
{
    QString out;
    for (int i = 0; i < inner.size(); ++i) {
        if (inner.at(i) == QLatin1Char('\\')) {
            if (i + 1 >= inner.size()) {
                *ok = false;
                return {};
            }
            const QChar next = inner.at(++i);
            if (next == QLatin1Char('n'))
                out += QLatin1Char('\n');
            else if (next == QLatin1Char('r'))
                out += QLatin1Char('\r');
            else if (next == QLatin1Char('t'))
                out += QLatin1Char('\t');
            else
                out += next;
        } else if (inner.at(i) == QLatin1Char('"')) {
            *ok = false;
            return {};
        } else {
            out += inner.at(i);
        }
    }
    *ok = true;
    return out;
}

QString escapeIni(const QString &value)
{
    QString out;
    for (const QChar character : value) {
        if (character == QLatin1Char('\\') || character == QLatin1Char('"')) {
            out += QLatin1Char('\\');
            out += character;
        } else if (character == QLatin1Char('\n')) {
            out += QLatin1String("\\n");
        } else if (character == QLatin1Char('\r')) {
            out += QLatin1String("\\r");
        } else if (character == QLatin1Char('\t')) {
            out += QLatin1String("\\t");
        } else {
            out += character;
        }
    }
    return out;
}

bool iniNeedsQuotes(const QString &value)
{
    if (value.isEmpty())
        return false;
    if (value.front().isSpace() || value.back().isSpace())
        return true;
    return value.contains(QLatin1Char(' ')) || value.contains(QLatin1Char('\t')) || value.contains(QLatin1Char('#'))
           || value.contains(QLatin1Char(';')) || value.contains(QLatin1Char('"')) || value.contains(QLatin1Char('='))
           || value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'));
}

QString encodeIniValue(const Node *node)
{
    const bool quote = node->quoted || iniNeedsQuotes(node->value);
    if (!quote)
        return node->value;
    return QLatin1Char('"') + escapeIni(node->value) + QLatin1Char('"');
}

Sheet failIni(int line, const QString &why)
{
    Sheet sheet;
    sheet.notation = Notation::Ini;
    sheet.line = line;
    sheet.column = 1;
    sheet.message = QStringLiteral("第 %1 行：%2").arg(line).arg(why);
    return sheet;
}

Sheet parseIni(const QString &text)
{
    Sheet sheet;
    sheet.notation = Notation::Ini;
    auto root = std::make_unique<Node>();
    root->kind = NodeKind::Document;
    root->line = 1;
    root->column = 1;
    QStringList lines = text.split(QLatin1Char('\n'));
    if (text.endsWith(QLatin1Char('\n')) && !lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();

    Node *section = nullptr;
    int nodes = 1;
    for (int i = 0; i < lines.size(); ++i) {
        const int lineNo = i + 1;
        QString raw = lines.at(i);
        if (raw.endsWith(QLatin1Char('\r')))
            raw.chop(1);
        const QString trimmed = raw.trimmed();
        if (++nodes > kMaxNodes)
            return failIni(lineNo, QStringLiteral("条目太多（超过 2 万）。请把文件拆小后再检查，以免窗口卡住。"));

        Node *parent = section != nullptr ? section : root.get();
        if (trimmed.isEmpty()) {
            auto blank = std::make_unique<Node>();
            blank->kind = NodeKind::Blank;
            blank->line = lineNo;
            blank->column = 1;
            parent->append(std::move(blank));
            continue;
        }
        if (trimmed.startsWith(QLatin1Char('#')) || trimmed.startsWith(QLatin1Char(';'))) {
            auto comment = std::make_unique<Node>();
            comment->kind = NodeKind::Comment;
            comment->spelling = trimmed.left(1);
            comment->value = trimmed;
            comment->line = lineNo;
            comment->column = 1;
            parent->append(std::move(comment));
            continue;
        }
        if (trimmed.startsWith(QLatin1Char('['))) {
            if (!trimmed.endsWith(QLatin1Char(']')) || trimmed.indexOf(QLatin1Char(']')) != trimmed.size() - 1)
                return failIni(lineNo, QStringLiteral("小节的名字需要写成 [server] 这样，右括号缺失。"));
            const QString name = trimmed.mid(1, trimmed.size() - 2).trimmed();
            if (name.isEmpty())
                return failIni(lineNo, QStringLiteral("小节的名字是空的。请写成 [server]。"));
            auto created = std::make_unique<Node>();
            created->kind = NodeKind::Section;
            created->name = name;
            created->line = lineNo;
            created->column = 1;
            section = root->append(std::move(created));
            continue;
        }
        const int eq = raw.indexOf(QLatin1Char('='));
        if (eq <= 0)
            return failIni(lineNo, QStringLiteral("这一行不是「名称=值」。请写成 port=8080 这样。"));
        const QString key = raw.left(eq).trimmed();
        QString val = raw.mid(eq + 1).trimmed();
        if (key.isEmpty())
            return failIni(lineNo, QStringLiteral("等号左边的名字是空的。请写成 port=8080 这样。"));
        bool quoted = false;
        if (val.size() >= 2 && val.front() == QLatin1Char('"') && val.back() == QLatin1Char('"')) {
            bool ok = false;
            const QString decoded = unescapeIni(val.mid(1, val.size() - 2), &ok);
            if (!ok)
                return failIni(lineNo, QStringLiteral("引号没有成对，或引号里的转义不完整。"));
            val = decoded;
            quoted = true;
        }
        auto field = std::make_unique<Node>();
        field->kind = NodeKind::Field;
        field->name = key;
        field->value = val;
        field->quoted = quoted;
        field->line = lineNo;
        field->column = qMax(1, raw.indexOf(key) + 1);
        parent->append(std::move(field));
    }

    sheet.root = std::move(root);
    sheet.ok = true;
    return sheet;
}

void appendJson(QString *out, const Node *node, int indent)
{
    const QString pad(indent * 2, QLatin1Char(' '));
    const QString nest((indent + 1) * 2, QLatin1Char(' '));
    switch (node->kind) {
    case NodeKind::Map:
        if (node->childCount() == 0) {
            *out += QLatin1String("{}");
            return;
        }
        *out += QLatin1String("{\n");
        for (int i = 0; i < node->childCount(); ++i) {
            const Node *child = node->children[static_cast<size_t>(i)].get();
            *out += nest;
            *out += QLatin1Char('"');
            *out += escapeJson(child->name);
            *out += QLatin1String("\": ");
            appendJson(out, child, indent + 1);
            if (i + 1 != node->childCount())
                *out += QLatin1Char(',');
            *out += QLatin1Char('\n');
        }
        *out += pad;
        *out += QLatin1Char('}');
        break;
    case NodeKind::List:
        if (node->childCount() == 0) {
            *out += QLatin1String("[]");
            return;
        }
        *out += QLatin1String("[\n");
        for (int i = 0; i < node->childCount(); ++i) {
            *out += nest;
            appendJson(out, node->children[static_cast<size_t>(i)].get(), indent + 1);
            if (i + 1 != node->childCount())
                *out += QLatin1Char(',');
            *out += QLatin1Char('\n');
        }
        *out += pad;
        *out += QLatin1Char(']');
        break;
    case NodeKind::Phrase:
        *out += QLatin1Char('"');
        *out += escapeJson(node->value);
        *out += QLatin1Char('"');
        break;
    case NodeKind::Quantity:
        *out += node->spelling.isEmpty() ? node->value : node->spelling;
        break;
    case NodeKind::Switch:
        *out += node->value == QLatin1String("false") ? QLatin1String("false") : QLatin1String("true");
        break;
    case NodeKind::Vacant:
        *out += QLatin1String("null");
        break;
    default:
        *out += QLatin1String("null");
        break;
    }
}

void appendXmlElement(QString *out, const Node *node, int indent)
{
    const QString pad(indent * 2, QLatin1Char(' '));
    *out += pad;
    *out += QLatin1Char('<');
    *out += node->name;
    QVector<const Node *> body;
    for (int i = 0; i < node->childCount(); ++i) {
        const Node *child = node->children[static_cast<size_t>(i)].get();
        if (child->kind == NodeKind::Attribute) {
            *out += QLatin1Char(' ');
            *out += child->name;
            *out += QLatin1String("=\"");
            *out += escapeXml(child->value, true);
            *out += QLatin1Char('"');
        } else {
            body.append(child);
        }
    }
    if (body.isEmpty()) {
        *out += QLatin1String("/>");
        return;
    }
    const bool inlineText = body.size() == 1 && body.at(0)->kind == NodeKind::Characters && !body.at(0)->cdata;
    if (inlineText) {
        *out += QLatin1Char('>');
        *out += escapeXml(body.at(0)->value, false);
        *out += QLatin1String("</");
        *out += node->name;
        *out += QLatin1Char('>');
        return;
    }
    *out += QLatin1String(">\n");
    for (const Node *child : body) {
        if (child->kind == NodeKind::Element) {
            appendXmlElement(out, child, indent + 1);
            *out += QLatin1Char('\n');
        } else if (child->kind == NodeKind::Characters) {
            *out += QString(indent * 2 + 2, QLatin1Char(' '));
            if (child->cdata) {
                QString safe = child->value;
                safe.replace(QLatin1String("]]>"), QLatin1String("]] >"));
                *out += QLatin1String("<![CDATA[");
                *out += safe;
                *out += QLatin1String("]]>");
            } else {
                *out += escapeXml(child->value, false);
            }
            *out += QLatin1Char('\n');
        } else if (child->kind == NodeKind::Comment) {
            *out += QString(indent * 2 + 2, QLatin1Char(' '));
            if (child->spelling == QLatin1String("pi")) {
                *out += QLatin1String("<?");
                *out += child->name;
                if (!child->value.isEmpty()) {
                    *out += QLatin1Char(' ');
                    *out += child->value;
                }
                *out += QLatin1String("?>");
            } else {
                QString safe = child->value;
                safe.replace(QLatin1String("--"), QLatin1String("- -"));
                *out += QLatin1String("<!--");
                *out += safe;
                *out += QLatin1String("-->");
            }
            *out += QLatin1Char('\n');
        }
    }
    *out += pad;
    *out += QLatin1String("</");
    *out += node->name;
    *out += QLatin1Char('>');
}

void appendIniNodes(QString *out, const Node *container)
{
    for (int i = 0; i < container->childCount(); ++i) {
        const Node *child = container->children[static_cast<size_t>(i)].get();
        switch (child->kind) {
        case NodeKind::Blank:
            *out += QLatin1Char('\n');
            break;
        case NodeKind::Comment:
            *out += child->value;
            *out += QLatin1Char('\n');
            break;
        case NodeKind::Field:
            *out += child->name;
            *out += QLatin1Char('=');
            *out += encodeIniValue(child);
            *out += QLatin1Char('\n');
            break;
        case NodeKind::Section:
            *out += QLatin1Char('[');
            *out += child->name;
            *out += QLatin1String("]\n");
            appendIniNodes(out, child);
            break;
        default:
            break;
        }
    }
}

} // namespace

QString notationCaption(Notation notation)
{
    switch (notation) {
    case Notation::Json:
        return QStringLiteral("JSON");
    case Notation::Xml:
        return QStringLiteral("XML");
    case Notation::Ini:
        return QStringLiteral("INI");
    case Notation::Automatic:
        return QStringLiteral("自动识别");
    }
    return QStringLiteral("未知");
}

QString nodeKindCaption(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Document:
        return QStringLiteral("文档");
    case NodeKind::Map:
        return QStringLiteral("对象");
    case NodeKind::List:
        return QStringLiteral("数组");
    case NodeKind::Phrase:
        return QStringLiteral("字符串");
    case NodeKind::Quantity:
        return QStringLiteral("数字");
    case NodeKind::Switch:
        return QStringLiteral("布尔");
    case NodeKind::Vacant:
        return QStringLiteral("空");
    case NodeKind::Element:
        return QStringLiteral("元素");
    case NodeKind::Attribute:
        return QStringLiteral("属性");
    case NodeKind::Characters:
        return QStringLiteral("文本");
    case NodeKind::Comment:
        return QStringLiteral("注释");
    case NodeKind::Section:
        return QStringLiteral("节");
    case NodeKind::Field:
        return QStringLiteral("键");
    case NodeKind::Blank:
        return QStringLiteral("空行");
    }
    return QStringLiteral("项");
}

QString nodeLabel(const Node *node)
{
    if (!node)
        return {};
    if (node->kind == NodeKind::Attribute)
        return QStringLiteral("@") + node->name;
    if (node->parent != nullptr && node->parent->kind == NodeKind::List)
        return QString::number(node->indexInParent());
    if (node->kind == NodeKind::Characters)
        return QStringLiteral("文本");
    if (node->kind == NodeKind::Comment)
        return node->spelling == QLatin1String("pi") ? node->name : QStringLiteral("注释");
    if (node->kind == NodeKind::Blank)
        return QStringLiteral("空行");
    if (node->name.isEmpty())
        return QStringLiteral("值");
    return node->name;
}

QString displayedValue(const Node *node)
{
    if (!node)
        return {};
    switch (node->kind) {
    case NodeKind::Quantity:
        return node->spelling.isEmpty() ? node->value : node->spelling;
    case NodeKind::Switch:
        return node->value == QLatin1String("false") ? QStringLiteral("false") : QStringLiteral("true");
    case NodeKind::Vacant:
        return QStringLiteral("null");
    case NodeKind::Phrase:
    case NodeKind::Field:
    case NodeKind::Attribute:
    case NodeKind::Characters:
    case NodeKind::Comment:
        return node->value;
    case NodeKind::Element: {
        bool nested = false;
        QString text;
        for (int i = 0; i < node->childCount(); ++i) {
            const Node *child = node->children[static_cast<size_t>(i)].get();
            if (child->kind == NodeKind::Element)
                nested = true;
            else if (child->kind == NodeKind::Characters)
                text += child->value;
        }
        if (!nested)
            return text;
        return QStringLiteral("%1 项").arg(node->childCount());
    }
    case NodeKind::Map:
    case NodeKind::List:
    case NodeKind::Section:
    case NodeKind::Document:
        return QStringLiteral("%1 项").arg(node->childCount());
    case NodeKind::Blank:
        return {};
    }
    return {};
}

bool isJsonNumber(const QString &text)
{
    if (text.isEmpty())
        return false;
    int index = 0;
    const int size = text.size();
    if (text.at(index) == QLatin1Char('-')) {
        ++index;
        if (index >= size)
            return false;
    }
    if (!isAsciiDigit(text.at(index)))
        return false;
    if (text.at(index) == QLatin1Char('0')) {
        ++index;
    } else {
        while (index < size && isAsciiDigit(text.at(index)))
            ++index;
    }
    if (index < size && text.at(index) == QLatin1Char('.')) {
        ++index;
        if (index >= size || !isAsciiDigit(text.at(index)))
            return false;
        while (index < size && isAsciiDigit(text.at(index)))
            ++index;
    }
    if (index < size && (text.at(index) == QLatin1Char('e') || text.at(index) == QLatin1Char('E'))) {
        ++index;
        if (index < size && (text.at(index) == QLatin1Char('+') || text.at(index) == QLatin1Char('-')))
            ++index;
        if (index >= size || !isAsciiDigit(text.at(index)))
            return false;
        while (index < size && isAsciiDigit(text.at(index)))
            ++index;
    }
    return index == size;
}

bool nodeNameEditable(const Node *node)
{
    if (node == nullptr || node->parent == nullptr)
        return false;
    if (node->parent->kind == NodeKind::List)
        return false;
    if (node->parent->kind == NodeKind::Map)
        return true;
    switch (node->kind) {
    case NodeKind::Element:
    case NodeKind::Attribute:
    case NodeKind::Section:
    case NodeKind::Field:
        return true;
    default:
        return false;
    }
}

bool nodeValueEditable(const Node *node)
{
    if (node == nullptr)
        return false;
    switch (node->kind) {
    case NodeKind::Phrase:
    case NodeKind::Quantity:
    case NodeKind::Switch:
    case NodeKind::Field:
    case NodeKind::Attribute:
    case NodeKind::Characters:
    case NodeKind::Comment:
        return true;
    case NodeKind::Element: {
        for (int i = 0; i < node->childCount(); ++i) {
            if (node->children[static_cast<size_t>(i)]->kind == NodeKind::Element)
                return false;
        }
        return true;
    }
    default:
        return false;
    }
}

bool isJsonScalar(const Node *node)
{
    if (node == nullptr)
        return false;
    switch (node->kind) {
    case NodeKind::Phrase:
    case NodeKind::Quantity:
    case NodeKind::Switch:
    case NodeKind::Vacant:
        break;
    default:
        return false;
    }
    if (node->parent == nullptr)
        return true;
    return node->parent->kind == NodeKind::Map || node->parent->kind == NodeKind::List;
}

Sheet parseSheet(const QString &text, Notation requested, const QString &suffix)
{
    QString body = text;
    if (body.startsWith(QChar(0xFEFF)))
        body.remove(0, 1);
    if (body.size() > kMaxChars) {
        Sheet sheet;
        sheet.line = 1;
        sheet.message = QStringLiteral(
                            "这份文本大约有 %1 个字符，一次展开会让窗口很难操作。请先拆成小于 1.5 MB 的文件。")
                            .arg(body.size());
        return sheet;
    }
    if (body.trimmed().isEmpty() && requested == Notation::Automatic && suffix.trimmed().isEmpty()) {
        Sheet sheet;
        sheet.line = 1;
        sheet.message = QStringLiteral("还没有内容。请输入 JSON、XML 或 INI。");
        return sheet;
    }

    Notation resolved = Notation::Json;
    QString message;
    if (!resolveNotation(body, requested, suffix, &resolved, &message)) {
        Sheet sheet;
        sheet.line = 1;
        sheet.message = message;
        return sheet;
    }
    switch (resolved) {
    case Notation::Xml:
        return parseXml(body);
    case Notation::Ini:
        return parseIni(body);
    case Notation::Json:
    case Notation::Automatic:
        return parseJson(body);
    }
    return parseJson(body);
}

QString composeSheet(const Sheet &sheet, QString *error)
{
    if (!sheet.root) {
        if (error)
            *error = QStringLiteral("没有可写回的结构。");
        return {};
    }
    QString out;
    switch (sheet.notation) {
    case Notation::Json:
        appendJson(&out, sheet.root.get(), 0);
        out += QLatin1Char('\n');
        break;
    case Notation::Xml:
        if (!sheet.prologue.isEmpty()) {
            out += sheet.prologue;
            out += QLatin1Char('\n');
        }
        for (int i = 0; i < sheet.root->childCount(); ++i) {
            const Node *child = sheet.root->children[static_cast<size_t>(i)].get();
            if (child->kind == NodeKind::Element) {
                appendXmlElement(&out, child, 0);
                out += QLatin1Char('\n');
            } else if (child->kind == NodeKind::Comment) {
                if (child->spelling == QLatin1String("pi")) {
                    out += QLatin1String("<?");
                    out += child->name;
                    if (!child->value.isEmpty()) {
                        out += QLatin1Char(' ');
                        out += child->value;
                    }
                    out += QLatin1String("?>\n");
                } else {
                    out += QLatin1String("<!--");
                    out += child->value;
                    out += QLatin1String("-->\n");
                }
            }
        }
        break;
    case Notation::Ini:
        appendIniNodes(&out, sheet.root.get());
        break;
    case Notation::Automatic:
        if (error)
            *error = QStringLiteral("这种格式还不能写回。");
        return {};
    }
    return out;
}

Node *findNode(Node *root, const QStringList &path)
{
    if (root == nullptr)
        return nullptr;
    Node *current = root;
    for (const QString &part : path) {
        Node *next = nullptr;
        for (int i = 0; i < current->childCount(); ++i) {
            Node *child = current->children[static_cast<size_t>(i)].get();
            if (nodeLabel(child) == part) {
                next = child;
                break;
            }
        }
        if (next == nullptr)
            return nullptr;
        current = next;
    }
    return current;
}

} // namespace tw
