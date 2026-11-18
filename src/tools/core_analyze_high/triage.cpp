#include "tools/core_analyze_high/triage.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

namespace tw {
namespace {

constexpr int kExcerptChars = 3500;

enum class FileKind
{
    Missing,
    NotRegular,
    Unreadable,
    Truncated,
    NotElf,
    Program,
    OtherElf,
    Core
};

struct SignalNote
{
    int code;
    QString token;
    QString headline;
    QString why;
    QString next;
};

struct Frame
{
    QString address;
    QString function;
    QString location;
};

struct Facts
{
    int signalCode = 0;
    QString signalToken;
    QString signalDetail;
    QString faultAddress;
    QString program;
    QVector<Frame> frames;
    int threadCount = 0;
    bool sawAbort = false;
    bool sawStackCanary = false;
    bool sawAllocator = false;
    bool symbolsThin = false;
    bool programMismatch = false;
};

struct Plan
{
    QString heading;
    QString purpose;
    QString program;
    QString fallbackName;
    QStringList arguments;
    int timeoutMs = 20000;
};

const QVector<SignalNote> &signalNotes()
{
    static const QVector<SignalNote> notes = {
        {4, QStringLiteral("SIGILL"), QStringLiteral("执行了当前机器无法识别的指令"),
         QStringLiteral("常见于程序和这台机器的架构不一致，或程序文件已损坏。"),
         QStringLiteral("请换用适合本机的安装包，或重新安装后再试。")},
        {5, QStringLiteral("SIGTRAP"), QStringLiteral("停在了调试用的断点"),
         QStringLiteral("程序被断点拦住。日常打开软件时通常不会这样。"),
         QStringLiteral("请把这份说明交给负责该软件的人。")},
        {6, QStringLiteral("SIGABRT"), QStringLiteral("程序主动中止了自己"),
         QStringLiteral("程序检查到无法继续的情况，于是自己停下来。"),
         QStringLiteral("请回忆停止前屏幕上有没有提示，把提示和这份说明一起交给负责该软件的人。")},
        {7, QStringLiteral("SIGBUS"), QStringLiteral("碰到了无效的存储位置"),
         QStringLiteral("程序访问的存储位置无效，有时是相关文件不完整或被截断。"),
         QStringLiteral("请确认相关文件已经完整保存，换一份完整文件后再试。")},
        {8, QStringLiteral("SIGFPE"), QStringLiteral("做了无法完成的算术"),
         QStringLiteral("程序做了不能完成的计算，例如用数字除以零。"),
         QStringLiteral("请检查刚才输入的数字是不是 0，或是否超出正常范围，换一组数值后再试。")},
        {11, QStringLiteral("SIGSEGV"), QStringLiteral("访问了不允许访问的内存"),
         QStringLiteral("程序去读或写一块现在不能使用的内存。"),
         QStringLiteral("请记下停止前的最后一次操作，并把这份说明交给负责该软件的人。")},
        {13, QStringLiteral("SIGPIPE"), QStringLiteral("往已经断开的连接继续写数据"),
         QStringLiteral("对方已经断开，程序还在发送内容。"),
         QStringLiteral("请确认网络或对方程序仍在运行，重新连接后再试。")},
        {24, QStringLiteral("SIGXCPU"), QStringLiteral("占用处理器的时间超过上限"),
         QStringLiteral("程序运行的时间超过了系统允许的上限，被系统停下。"),
         QStringLiteral("请稍后再试。如果反复出现，请把这份说明交给负责该软件的人。")},
        {25, QStringLiteral("SIGXFSZ"), QStringLiteral("写出的文件超过了大小限制"),
         QStringLiteral("程序想写入的内容超过了系统允许的大小。"),
         QStringLiteral("请清理磁盘空间，或请管理员放宽文件大小限制后再试。")},
        {31, QStringLiteral("SIGSYS"), QStringLiteral("调用了系统不支持的功能"),
         QStringLiteral("程序向系统发起了当前系统不认识的请求。"),
         QStringLiteral("请确认软件版本和系统匹配，并把这份说明交给负责该软件的人。")},
    };
    return notes;
}

const SignalNote *noteByCode(int code)
{
    for (const SignalNote &note : signalNotes()) {
        if (note.code == code)
            return &note;
    }
    return nullptr;
}

const SignalNote *noteByToken(const QString &token)
{
    for (const SignalNote &note : signalNotes()) {
        if (note.token == token)
            return &note;
    }
    return nullptr;
}

QString locateDefault(const QString &name)
{
    return QStandardPaths::findExecutable(name);
}

bool hasControlCharacter(const QString &path)
{
    for (const QChar ch : path) {
        if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r') || ch.unicode() < 0x20)
            return true;
    }
    return false;
}

QString absolutePath(const QString &path)
{
    return path.isEmpty() ? path : QFileInfo(path).absoluteFilePath();
}

FileKind classifyFile(const QString &path, QString *detail)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        *detail = QStringLiteral("找不到这个文件：%1").arg(path);
        return FileKind::Missing;
    }
    if (!info.isFile()) {
        *detail = QStringLiteral("请选择文件，而不是文件夹。");
        return FileKind::NotRegular;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *detail = QStringLiteral("文件打不开：%1").arg(file.errorString());
        return FileKind::Unreadable;
    }
    const QByteArray bytes = file.read(64);
    const auto *raw = reinterpret_cast<const unsigned char *>(bytes.constData());
    const bool elfMagic = bytes.size() >= 4 && raw[0] == 0x7f && raw[1] == 'E' && raw[2] == 'L'
        && raw[3] == 'F';
    if (bytes.size() < 20) {
        if (!elfMagic) {
            *detail = QStringLiteral("文件开头不是 ELF，通常是普通文本、图片或压缩包。");
            return FileKind::NotElf;
        }
        *detail = QStringLiteral("文件太短，放不下完整的 ELF 头。");
        return FileKind::Truncated;
    }
    if (!elfMagic) {
        *detail = QStringLiteral("文件开头不是 ELF，通常是普通文本、图片或压缩包。");
        return FileKind::NotElf;
    }
    const int klass = raw[4];
    const int endian = raw[5];
    if ((klass != 1 && klass != 2) || (endian != 1 && endian != 2)) {
        *detail = QStringLiteral("ELF 头不完整，无法判断它是不是 core 记录。");
        return FileKind::NotElf;
    }
    const auto read16 = [&](int offset) {
        if (endian == 1)
            return int(raw[offset]) | (int(raw[offset + 1]) << 8);
        return (int(raw[offset]) << 8) | int(raw[offset + 1]);
    };
    const int type = read16(16);
    if (type == 4)
        return FileKind::Core;
    if (type == 2 || type == 3) {
        *detail = QStringLiteral("这是程序文件，请把它填到“程序文件”，core 文件另选。");
        return FileKind::Program;
    }
    *detail = QStringLiteral("这是 ELF 文件，但不是 core 记录。");
    return FileKind::OtherElf;
}

QString gdbDirectoryCommand(const QString &directory)
{
    QString escaped;
    escaped.reserve(directory.size());
    for (const QChar ch : directory) {
        if (ch == QLatin1Char('\\') || ch == QLatin1Char('"'))
            escaped += QLatin1Char('\\');
        escaped += ch;
    }
    return QStringLiteral("set debug-file-directory \"%1\"").arg(escaped);
}

QString numberLines(const QStringList &items)
{
    QStringList lines;
    for (int i = 0; i < items.size(); ++i)
        lines << QStringLiteral("%1. %2").arg(i + 1).arg(items.at(i));
    return lines.join(QLatin1Char('\n'));
}

QString situationFor(const ArgvResult &result)
{
    switch (result.state) {
    case ArgvState::Finished:
        return result.exitCode == 0
            ? QStringLiteral("已执行")
            : QStringLiteral("已执行（返回码 %1）").arg(result.exitCode);
    case ArgvState::TimedOut:
        return QStringLiteral("已中止：%1").arg(result.detail);
    case ArgvState::StartFailed:
        return QStringLiteral("没有跑起来：%1").arg(result.detail);
    case ArgvState::Stopped:
        return QStringLiteral("已取消");
    case ArgvState::NotFound:
        return QStringLiteral("未执行");
    }
    return QStringLiteral("未执行");
}

QString clipExcerpt(QString text, bool clipped)
{
    text = text.trimmed();
    if (text.size() > kExcerptChars) {
        text.truncate(kExcerptChars);
        clipped = true;
    }
    if (clipped)
        text += QStringLiteral("\n…（后面还有输出，已截断）");
    return text;
}

TriageStep stepFromPlan(const Plan &plan)
{
    TriageStep step;
    step.heading = plan.heading;
    step.purpose = plan.purpose;
    const QString shown = plan.program.isEmpty() ? plan.fallbackName : plan.program;
    step.commandLine = joinArgvForDisplay(shown, plan.arguments);
    return step;
}

QString signalLine(const TriageOutcome &outcome)
{
    if (!outcome.signalToken.isEmpty() && outcome.signalCode > 0)
        return QStringLiteral("%1（%2）").arg(outcome.signalToken).arg(outcome.signalCode);
    if (!outcome.signalToken.isEmpty())
        return outcome.signalToken;
    if (outcome.signalCode > 0)
        return QString::number(outcome.signalCode);
    return QStringLiteral("未能读出");
}

QString composeDocument(const TriageOutcome &outcome)
{
    QString text;
    text += QStringLiteral("文件分析high · 说明\n");
    text += QStringLiteral("记录时间：");
    text += QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    text += QStringLiteral("\n命令原文保持英文，方便和中文释义对照。写在下面的命令与这次实际执行的参数一致。\n\n");

    text += QStringLiteral("■ 结论\n");
    text += QStringLiteral("信号：");
    text += signalLine(outcome);
    text += QStringLiteral("\n报错信息：");
    text += outcome.fault.isEmpty() ? QStringLiteral("无") : outcome.fault;
    text += QStringLiteral("\n所在步骤：");
    text += outcome.place.isEmpty() ? QStringLiteral("无") : outcome.place;
    text += QStringLiteral("\n可能原因：");
    text += outcome.likely.isEmpty() ? QStringLiteral("无") : outcome.likely;
    text += QStringLiteral("\n建议解决方案：\n");
    text += outcome.advice.isEmpty() ? QStringLiteral("无\n") : outcome.advice + QLatin1Char('\n');
    text += QLatin1Char('\n');

    text += QStringLiteral("■ 分步操作\n");
    text += QStringLiteral("core 文件是程序异常退出时操作系统留下的现场记录。");
    text += QStringLiteral("按下面的顺序看：每一步都有要运行的命令、这次是否真的跑过、关键输出，以及这些输出在说什么。\n\n");
    if (outcome.steps.isEmpty()) {
        text += CoreTriage::primer();
        text += QLatin1Char('\n');
    }
    for (int i = 0; i < outcome.steps.size(); ++i) {
        const TriageStep &step = outcome.steps.at(i);
        text += QStringLiteral("【%1】%2\n").arg(i + 1).arg(step.heading);
        text += QStringLiteral("要解决的问题：");
        text += step.purpose;
        text += QStringLiteral("\n实际命令：\n    ");
        text += step.commandLine;
        text += QStringLiteral("\n本次情况：");
        text += step.situation.isEmpty() ? QStringLiteral("未执行") : step.situation;
        text += QLatin1Char('\n');
        if (!step.excerpt.isEmpty()) {
            text += QStringLiteral("关键输出：\n");
            text += step.excerpt;
            text += QLatin1Char('\n');
        }
        if (!step.meaning.isEmpty()) {
            text += QStringLiteral("中文释义：");
            text += step.meaning;
            text += QLatin1Char('\n');
        }
        text += QLatin1Char('\n');
    }

    text += QStringLiteral("■ 本次限制\n");
    if (outcome.caveats.isEmpty()) {
        text += QStringLiteral("没有额外限制。上面列出的就是这次用到的命令。\n");
    } else {
        for (const QString &caveat : outcome.caveats) {
            text += QStringLiteral("- ");
            text += caveat;
            text += QLatin1Char('\n');
        }
    }
    return text;
}

void rememberProgram(Facts *facts, const QString &raw)
{
    const QString text = raw.trimmed();
    if (text.isEmpty())
        return;
    const bool incomingPath = text.contains(QLatin1Char('/'));
    const bool heldPath = facts->program.contains(QLatin1Char('/'));
    if (facts->program.isEmpty() || (!heldPath && incomingPath))
        facts->program = text;
}

void absorbFileText(const QString &text, Facts *facts)
{
    const QRegularExpression execfn(QStringLiteral("execfn:\\s*'([^']*)'"));
    const QRegularExpression fromName(QStringLiteral("from\\s+'([^']*)'"));
    const QRegularExpressionMatch execMatch = execfn.match(text);
    if (execMatch.hasMatch())
        rememberProgram(facts, execMatch.captured(1));
    const QRegularExpressionMatch fromMatch = fromName.match(text);
    if (fromMatch.hasMatch())
        rememberProgram(facts, fromMatch.captured(1));
}

int firstPositive(const QString &text, const QString &pattern)
{
    const QRegularExpression expression(pattern);
    QRegularExpressionMatchIterator it = expression.globalMatch(text);
    while (it.hasNext()) {
        const int value = it.next().captured(1).toInt();
        if (value > 0)
            return value;
    }
    return 0;
}

void absorbNotes(const QString &text, Facts *facts)
{
    const int signo = firstPositive(text, QStringLiteral("\\bsi_signo:\\s*(\\d+)"));
    const int cursig = firstPositive(text, QStringLiteral("\\bcursig:\\s*(\\d+)"));
    if (facts->signalCode == 0)
        facts->signalCode = signo > 0 ? signo : cursig;

    const QRegularExpression address(QStringLiteral("\\bsi_addr:\\s*(\\S+)"));
    const QRegularExpressionMatch addressMatch = address.match(text);
    if (addressMatch.hasMatch() && facts->faultAddress.isEmpty())
        facts->faultAddress = addressMatch.captured(1);

    const QRegularExpression psargs(QStringLiteral("\\bpsargs:\\s*(.+)"));
    const QRegularExpression fname(QStringLiteral("\\bfname:\\s*(\\S+)"));
    const QRegularExpressionMatch psMatch = psargs.match(text);
    if (psMatch.hasMatch())
        rememberProgram(facts, psMatch.captured(1));
    const QRegularExpressionMatch nameMatch = fname.match(text);
    if (nameMatch.hasMatch())
        rememberProgram(facts, nameMatch.captured(1));
}

bool nullAddress(const QString &address)
{
    const QString value = address.trimmed().toLower();
    return value == QLatin1String("0") || value == QLatin1String("0x0")
        || value == QLatin1String("(nil)") || value == QLatin1String("nil")
        || value == QLatin1String("0x0000000000000000");
}

bool unnamed(const QString &function)
{
    const QString text = function.trimmed();
    return text.isEmpty() || text == QLatin1String("??") || text == QLatin1String("?");
}

void splitFunction(const QString &rest, QString *function, QString *location)
{
    const int atPos = rest.lastIndexOf(QStringLiteral(" at "));
    const int fromPos = rest.lastIndexOf(QStringLiteral(" from "));
    int cut = -1;
    QString where;
    if (atPos >= 0 && (fromPos < 0 || atPos > fromPos)) {
        cut = atPos;
        where = rest.mid(atPos + 4).trimmed();
    } else if (fromPos >= 0) {
        cut = fromPos;
        where = rest.mid(fromPos + 6).trimmed();
    }
    QString name = (cut >= 0 ? rest.left(cut) : rest).trimmed();
    if (name.endsWith(QLatin1Char(')'))) {
        const int paren = name.lastIndexOf(QStringLiteral(" ("));
        if (paren > 0)
            name = name.left(paren).trimmed();
    }
    *function = name;
    *location = where;
}

Frame parseFrame(const QString &line)
{
    Frame frame;
    const QRegularExpression addressed(
        QStringLiteral("^#\\d+\\s+(0x[0-9a-fA-F]+)\\s+in\\s+(.*)$"));
    const QRegularExpression bare(QStringLiteral("^#\\d+\\s+(\\S.*)$"));
    const QRegularExpressionMatch withAddress = addressed.match(line);
    if (withAddress.hasMatch()) {
        frame.address = withAddress.captured(1);
        splitFunction(withAddress.captured(2), &frame.function, &frame.location);
        return frame;
    }
    const QRegularExpressionMatch plain = bare.match(line);
    if (plain.hasMatch())
        splitFunction(plain.captured(1), &frame.function, &frame.location);
    return frame;
}

bool namesOneOf(const QString &function, const QStringList &needles)
{
    for (const QString &needle : needles) {
        if (function == needle || function.endsWith(QLatin1Char('_') + needle)
            || function.endsWith(QStringLiteral("::") + needle)) {
            return true;
        }
    }
    return false;
}

void absorbDebugger(const QString &text, Facts *facts)
{
    const QRegularExpression terminated(
        QStringLiteral("terminated with signal\\s+(SIG[A-Z0-9]+|\\d+)\\s*,\\s*([^\\r\\n]+)"));
    const QRegularExpressionMatch term = terminated.match(text);
    if (term.hasMatch()) {
        const QString token = term.captured(1);
        QString detail = term.captured(2).trimmed();
        if (detail.endsWith(QLatin1Char('.')))
            detail.chop(1);
        facts->signalDetail = detail;
        if (token.startsWith(QLatin1String("SIG")))
            facts->signalToken = token;
        else
            facts->signalCode = token.toInt();
    }
    if (facts->signalToken.isEmpty()) {
        const QRegularExpression named(QStringLiteral("\\b(SIG[A-Z][A-Z0-9]+)\\b"));
        const QRegularExpressionMatch match = named.match(text);
        if (match.hasMatch())
            facts->signalToken = match.captured(1);
    }

    const QRegularExpression generated(QStringLiteral("Core was generated by `([^`]*)'"));
    const QRegularExpressionMatch generatedMatch = generated.match(text);
    if (generatedMatch.hasMatch())
        rememberProgram(facts, generatedMatch.captured(1));

    if (text.contains(QLatin1String("core file may not match"), Qt::CaseInsensitive)
        || text.contains(QLatin1String("exec file is newer"), Qt::CaseInsensitive)) {
        facts->programMismatch = true;
    }
    if (text.contains(QLatin1String("No symbol table"), Qt::CaseInsensitive)
        || text.contains(QLatin1String("no debugging symbols"), Qt::CaseInsensitive)) {
        facts->symbolsThin = true;
    }

    QVector<QVector<Frame>> chunks;
    QVector<Frame> current;
    int threads = 0;
    auto flush = [&]() {
        if (!current.isEmpty())
            chunks.append(current);
        current.clear();
    };
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("Thread "))) {
            ++threads;
            flush();
            continue;
        }
        if (!line.startsWith(QLatin1Char('#')))
            continue;
        const Frame frame = parseFrame(line);
        if (frame.function.isEmpty() && frame.address.isEmpty())
            continue;
        const bool frameZero = line.startsWith(QLatin1String("#0"))
            && (line.size() == 2 || !line.at(2).isDigit());
        if (!current.isEmpty() && frameZero)
            flush();
        current.append(frame);
    }
    flush();
    facts->threadCount = threads;

    int best = -1;
    for (int i = 0; i < chunks.size(); ++i) {
        if (best < 0 || chunks.at(i).size() > chunks.at(best).size())
            best = i;
    }
    if (best >= 0)
        facts->frames = chunks.at(best);

    for (const Frame &frame : facts->frames) {
        if (namesOneOf(frame.function, {QStringLiteral("abort"), QStringLiteral("raise")}))
            facts->sawAbort = true;
        if (frame.function.contains(QLatin1String("stack_chk_fail")))
            facts->sawStackCanary = true;
        if (namesOneOf(frame.function, {QStringLiteral("malloc"), QStringLiteral("free")})
            || frame.function.contains(QLatin1String("operator delete"))
            || frame.function.contains(QLatin1String("operator new"))) {
            facts->sawAllocator = true;
        }
    }
    if (!facts->frames.isEmpty() && unnamed(facts->frames.first().function))
        facts->symbolsThin = true;
}

void reconcileSignal(Facts *facts)
{
    if (!facts->signalToken.isEmpty()) {
        if (const SignalNote *note = noteByToken(facts->signalToken))
            facts->signalCode = note->code;
    } else if (facts->signalCode > 0) {
        if (const SignalNote *note = noteByCode(facts->signalCode))
            facts->signalToken = note->token;
    }
}

const SignalNote *noteFor(const Facts &facts)
{
    if (const SignalNote *byToken = noteByToken(facts.signalToken))
        return byToken;
    return noteByCode(facts.signalCode);
}

QString placeText(const Facts &facts)
{
    QStringList parts;
    if (!facts.program.isEmpty())
        parts << QStringLiteral("当时运行的是“%1”。").arg(facts.program);
    if (facts.threadCount > 1) {
        parts << QStringLiteral("记录里有 %1 条同时进行的事，下面说的是出问题的那一条。")
                     .arg(facts.threadCount);
    }
    if (facts.frames.isEmpty()) {
        parts << QStringLiteral("这次没有得到程序停下前走过的步骤。");
        return parts.join(QString());
    }
    const Frame &top = facts.frames.first();
    if (unnamed(top.function)) {
        if (top.address.isEmpty())
            parts << QStringLiteral("程序已经停下，但看不出停在哪一步。");
        else
            parts << QStringLiteral("程序停在地址 %1，当前看不出步骤名。").arg(top.address);
    } else {
        parts << QStringLiteral("程序停在“%1”这一步。").arg(top.function);
        if (!top.location.isEmpty())
            parts << QStringLiteral("对应位置：%1。").arg(top.location);
    }
    QStringList earlier;
    const int limit = facts.frames.size() < 4 ? facts.frames.size() : 4;
    for (int i = 1; i < limit; ++i) {
        const QString &name = facts.frames.at(i).function;
        earlier << (unnamed(name) ? QStringLiteral("未知步骤") : name);
    }
    if (!earlier.isEmpty())
        parts << QStringLiteral("更早经过：%1。").arg(earlier.join(QStringLiteral(" → ")));
    return parts.join(QString());
}

QString faultText(const Facts &facts)
{
    if (const SignalNote *note = noteFor(facts)) {
        QString text = QStringLiteral("程序因为“%1”而停止").arg(note->headline);
        if (!facts.signalToken.isEmpty())
            text += QStringLiteral("（信号 %1，编号 %2）").arg(facts.signalToken).arg(facts.signalCode);
        else
            text += QStringLiteral("（编号 %1）").arg(facts.signalCode);
        text += QStringLiteral("。");
        return text;
    }
    if (!facts.frames.isEmpty())
        return QStringLiteral("程序已经停止。这次没有读出系统信号，停下的位置写在下面。");
    if (!facts.signalToken.isEmpty())
        return QStringLiteral("程序因为系统信号 %1 而停止。").arg(facts.signalToken);
    if (facts.signalCode > 0)
        return QStringLiteral("程序因为系统信号 %1 而停止。").arg(facts.signalCode);
    return QStringLiteral("这是一份 core 文件，但这次没能读出程序为何停止。");
}

QString likelyText(const Facts &facts)
{
    QStringList parts;
    if (const SignalNote *note = noteFor(facts))
        parts << note->why;
    else
        parts << QStringLiteral("这次只能确认程序异常停止，具体原因需要对照下面的原始输出。");
    if (facts.signalCode == 11 && nullAddress(facts.faultAddress)) {
        parts << QStringLiteral("它访问的位置是空的（地址为 0），常见于数据还没准备好就被拿去使用。");
    }
    if (facts.sawStackCanary)
        parts << QStringLiteral("写入的内容超出了预留空间，把旁边的数据冲坏了。");
    if (facts.sawAllocator) {
        parts << QStringLiteral("问题出在整理内存时，常见于同一块内存被释放了两次，或还在使用已经释放的内存。");
    }
    if (facts.sawAbort && facts.signalCode != 6)
        parts << QStringLiteral("调用过程里出现了程序自己要求停止的步骤。");
    if (facts.programMismatch)
        parts << QStringLiteral("填入的程序文件和这份记录对不上，步骤名可能不准。");
    if (facts.symbolsThin) {
        parts << QStringLiteral("有些步骤只剩地址、没有名称。若手边有调试符号或未裁剪的程序文件，填上后可以重新分析。");
    }
    return parts.join(QString());
}

QStringList adviceLines(const Facts &facts, bool gdbMissing)
{
    QStringList items;
    if (const SignalNote *note = noteFor(facts))
        items << note->next;
    else
        items << QStringLiteral("请把导出的说明交给负责该软件的人。");
    items << QStringLiteral("请先不要删除这份 core 文件，后面排查还要用。");
    if (facts.symbolsThin) {
        items << QStringLiteral("如果手边有调试符号或未裁剪的程序文件，填到上方后重新分析，步骤名会更完整。");
    }
    if (gdbMissing)
        items << QStringLiteral("请安装 gdb 后重新分析，才能看到停在哪一步。");
    return items;
}

QString fileMeaning(const QString &output, bool coreFile)
{
    const bool saysCore = output.contains(QLatin1String("core file"), Qt::CaseInsensitive)
        || output.contains(QLatin1String("core dump"), Qt::CaseInsensitive)
        || output.contains(QStringLiteral("核心转储"));
    if (saysCore) {
        return QStringLiteral("输出里出现 core file（或“核心转储”），表示这是程序异常退出时留下的现场记录，可以继续看它为什么停止。");
    }
    if (coreFile) {
        return QStringLiteral("文件头已经确认这是 core 记录。上面是系统对文件类型的描述，用词可能和 core file 不完全一样。");
    }
    return QStringLiteral("输出里没有 core file，说明它不是崩溃现场。分析停在这一步，不会再对它运行调试器。");
}

QString notesMeaning(const QString &output, const Facts &facts)
{
    if (output.contains(QLatin1String("si_signo")) || output.contains(QLatin1String("cursig"))) {
        QString text = QStringLiteral("si_signo 或 cursig 后面的数字是停止信号。");
        if (facts.signalCode > 0) {
            text += QStringLiteral("这次读到的是 %1").arg(facts.signalCode);
            if (!facts.signalToken.isEmpty())
                text += QStringLiteral("，也就是 %1").arg(facts.signalToken);
            text += QStringLiteral("。");
        }
        if (nullAddress(facts.faultAddress))
            text += QStringLiteral(" si_addr 为 0，表示用到了空的位置。");
        else if (!facts.faultAddress.isEmpty())
            text += QStringLiteral(" si_addr 是出问题的位置。");
        return text;
    }
    if (output.contains(QLatin1String("Cannot decode"), Qt::CaseInsensitive)
        || output.contains(QLatin1String("description data"), Qt::CaseInsensitive)) {
        return QStringLiteral("这条命令没有把记录翻译成可读字段。有的 readelf 解不开 64 位记录，停止原因改看后面的调试器输出。");
    }
    return QStringLiteral("这次没有从记录里读出停止信号。若后面的调试器有输出，以调试器为准。");
}

QString unstripMeaning(const QString &output)
{
    QString text = QStringLiteral("这个命令用来看程序和对照信息是否齐全。缺少对照信息时，仍能判断停止信号，但步骤名可能只显示为问号。");
    if (output.contains(QLatin1String(" - ")) || output.contains(QLatin1String(" . ")))
        text += QStringLiteral(" 输出里的 “-” 或 “.” 表示对应的程序或对照信息缺失。");
    return text;
}

QString debuggerMeaning(const Facts &facts)
{
    QString text = QStringLiteral("“Program terminated with signal”表示程序因该信号停止。以 #0 开头的一行是停下的位置，下面几行是更早的步骤。问号表示当前没有步骤名。");
    if (facts.programMismatch)
        text += QStringLiteral(" 输出还提到程序文件和记录对不上，请换用生成这份记录时的那个程序。");
    return text;
}

} // namespace

QString CoreTriage::primer()
{
    return QStringLiteral(
        "分步操作\n"
        "\n"
        "core 文件是程序异常退出时，操作系统留下的现场记录。可以按下面四步看懂大致原因。\n"
        "\n"
        "1. 辨认文件\n"
        "命令：file -b -- <core 文件>\n"
        "中文释义：输出里出现 core file，才是崩溃现场。普通文档、图片或压缩包不会出现这几个词。\n"
        "\n"
        "2. 读取停止信号\n"
        "命令：eu-readelf --notes --wide -- <core 文件>\n"
        "若没有 eu-readelf，改用：readelf -n -W -- <core 文件>\n"
        "中文释义：si_signo 或 cursig 后面的数字是停止信号。11 表示访问了不允许访问的内存，6 表示程序自己中止。\n"
        "\n"
        "3. 核对调试信息\n"
        "命令：eu-unstrip -n --core <core 文件>\n"
        "中文释义：用来看有没有把地址翻译成步骤名的对照信息。没有它也能知道信号，只是步骤名可能看不到。\n"
        "\n"
        "4. 查看停在哪一步\n"
        "命令：gdb --batch --nx --quiet -iex \"set pagination off\" -iex \"set confirm off\" -c <core 文件> -ex \"thread apply all bt\"\n"
        "填写了程序文件时，把程序路径放在 -c 前面。调试符号若是目录，再加上 -iex \"set debug-file-directory <目录>\"。\n"
        "中文释义：Program terminated with signal 后面是停止原因。以 #0 开头的一行是停下的位置。\n"
        "\n"
        "选好文件后点击“开始分析”。上面的命令会实际执行，并在每条命令下附上这次的输出和中文释义。\n");
}

TriageOutcome CoreTriage::run(const TriageInput &input, ArgvHost &host, const ToolLocator &locator,
                              const std::atomic<bool> *stop,
                              const std::function<void(const QString &)> &note)
{
    const ToolLocator locate = locator ? locator : ToolLocator(locateDefault);
    const auto halted = [&]() { return stop && stop->load(); };

    TriageOutcome outcome;
    Facts facts;

    auto publish = [&]() { outcome.document = composeDocument(outcome); };
    auto fail = [&](const QString &fault, const QString &likely, const QStringList &advice) {
        outcome.accepted = false;
        outcome.fault = fault;
        outcome.likely = likely;
        if (outcome.place.isEmpty())
            outcome.place = QStringLiteral("没有继续查看程序停下的位置。");
        outcome.advice = numberLines(advice);
        publish();
    };

    if (halted()) {
        outcome.stoppedEarly = true;
        fail(QStringLiteral("分析已取消。"), QStringLiteral("分析在开始前被取消了。"),
             {QStringLiteral("需要的话可以重新开始。")});
        return outcome;
    }

    const QString coreInput = input.corePath.trimmed();
    if (coreInput.isEmpty()) {
        fail(QStringLiteral("请先选择一个 core 文件。"),
             QStringLiteral("还没有选择要分析的文件。"),
             {QStringLiteral("点击“浏览”，选择程序异常退出时留下的 core 文件。")});
        return outcome;
    }
    if (hasControlCharacter(coreInput) || hasControlCharacter(input.programPath)
        || hasControlCharacter(input.debugPath)) {
        fail(QStringLiteral("路径里不能包含换行或控制字符。"),
             QStringLiteral("这样的路径无法安全地传给分析命令。"),
             {QStringLiteral("请换成普通文件路径后再试。")});
        return outcome;
    }

    const QString corePath = absolutePath(coreInput);
    QString programPath = input.programPath.trimmed().isEmpty()
        ? QString()
        : absolutePath(input.programPath.trimmed());
    QString debugPath = input.debugPath.trimmed().isEmpty()
        ? QString()
        : absolutePath(input.debugPath.trimmed());

    if (!programPath.isEmpty() && !QFileInfo(programPath).isFile()) {
        outcome.caveats << QStringLiteral("填写的程序文件找不到，已改为只根据 core 文件分析：%1")
                               .arg(programPath);
        programPath.clear();
    }
    bool debugIsDir = false;
    bool debugIsFile = false;
    if (!debugPath.isEmpty()) {
        const QFileInfo debugInfo(debugPath);
        if (debugInfo.isDir()) {
            debugIsDir = true;
        } else if (debugInfo.isFile()) {
            debugIsFile = true;
        } else {
            outcome.caveats << QStringLiteral("填写的调试符号找不到，已忽略：%1").arg(debugPath);
            debugPath.clear();
        }
    }

    const QString fileBin = locate(QStringLiteral("file"));
    QString notesBin = locate(QStringLiteral("eu-readelf"));
    QString notesName = QStringLiteral("eu-readelf");
    QStringList notesArgs = {QStringLiteral("--notes"), QStringLiteral("--wide"), QStringLiteral("--"),
                             corePath};
    const bool euReadelfMissing = notesBin.isEmpty();
    if (euReadelfMissing) {
        notesBin = locate(QStringLiteral("readelf"));
        notesName = QStringLiteral("readelf");
        notesArgs = QStringList{QStringLiteral("-n"), QStringLiteral("-W"), QStringLiteral("--"), corePath};
    }
    const QString unstripBin = locate(QStringLiteral("eu-unstrip"));
    const QString gdbBin = locate(QStringLiteral("gdb"));

    Plan filePlan;
    filePlan.heading = QStringLiteral("辨认这是不是 core 文件");
    filePlan.purpose = QStringLiteral("判断选中的文件是不是程序异常退出时留下的现场记录。");
    filePlan.program = fileBin;
    filePlan.fallbackName = QStringLiteral("file");
    filePlan.arguments = QStringList{QStringLiteral("-b"), QStringLiteral("--"), corePath};
    filePlan.timeoutMs = 8000;

    Plan notesPlan;
    notesPlan.heading = QStringLiteral("读取停止信号");
    notesPlan.purpose = QStringLiteral("从现场记录里取出程序因何停止，以及当时在跑哪个程序。");
    notesPlan.program = notesBin;
    notesPlan.fallbackName = notesName;
    notesPlan.arguments = notesArgs;
    notesPlan.timeoutMs = 20000;

    Plan unstripPlan;
    unstripPlan.heading = QStringLiteral("核对调试信息");
    unstripPlan.purpose = QStringLiteral("查看有没有把地址翻译成步骤名所需的对照信息。");
    unstripPlan.program = unstripBin;
    unstripPlan.fallbackName = QStringLiteral("eu-unstrip");
    unstripPlan.arguments = QStringList{QStringLiteral("-n"), QStringLiteral("--core"), corePath};
    if (!programPath.isEmpty())
        unstripPlan.arguments << QStringLiteral("-e") << programPath;
    unstripPlan.timeoutMs = 15000;

    Plan gdbPlan;
    gdbPlan.heading = QStringLiteral("查看停在哪一步");
    gdbPlan.purpose = QStringLiteral("用调试器打开现场记录，看停止信号和停下前走过的步骤。");
    gdbPlan.program = gdbBin;
    gdbPlan.fallbackName = QStringLiteral("gdb");
    gdbPlan.arguments = QStringList{QStringLiteral("--batch"), QStringLiteral("--nx"), QStringLiteral("--quiet"),
                                    QStringLiteral("-iex"), QStringLiteral("set pagination off"),
                                    QStringLiteral("-iex"), QStringLiteral("set confirm off")};
    if (debugIsDir)
        gdbPlan.arguments << QStringLiteral("-iex") << gdbDirectoryCommand(debugPath);
    if (debugIsFile)
        gdbPlan.arguments << QStringLiteral("-s") << debugPath;
    if (!programPath.isEmpty())
        gdbPlan.arguments << programPath;
    gdbPlan.arguments << QStringLiteral("-c") << corePath;
    gdbPlan.arguments << QStringLiteral("-ex") << QStringLiteral("thread apply all bt");
    gdbPlan.timeoutMs = 45000;

    TriageStep fileStep = stepFromPlan(filePlan);
    TriageStep notesStep = stepFromPlan(notesPlan);
    TriageStep unstripStep = stepFromPlan(unstripPlan);
    TriageStep gdbStep = stepFromPlan(gdbPlan);

    auto runPlan = [&](const Plan &plan, TriageStep *step) -> ArgvResult {
        if (halted()) {
            step->situation = QStringLiteral("已取消");
            outcome.stoppedEarly = true;
            ArgvResult stopped;
            stopped.state = ArgvState::Stopped;
            stopped.detail = QStringLiteral("已取消");
            return stopped;
        }
        if (plan.program.isEmpty()) {
            step->situation = QStringLiteral("未执行：本机没有找到 %1").arg(plan.fallbackName);
            ArgvResult missing;
            missing.state = ArgvState::NotFound;
            missing.detail = step->situation;
            return missing;
        }
        if (note)
            note(step->heading);
        ArgvTask task;
        task.program = plan.program;
        task.arguments = plan.arguments;
        task.directory = input.directory;
        task.timeoutMs = plan.timeoutMs;
        const ArgvResult result = host.run(task);
        step->invoked = result.state == ArgvState::Finished || result.state == ArgvState::TimedOut
            || result.state == ArgvState::Stopped;
        step->situation = situationFor(result);
        step->excerpt = clipExcerpt(result.text, result.clipped);
        if (result.state == ArgvState::Stopped)
            outcome.stoppedEarly = true;
        return result;
    };

    auto cancelRest = [&](TriageStep *first, TriageStep *second, TriageStep *third) {
        const QString situation = QStringLiteral("未执行：分析已取消");
        if (first && first->situation.isEmpty())
            first->situation = situation;
        if (second && second->situation.isEmpty())
            second->situation = situation;
        if (third && third->situation.isEmpty())
            third->situation = situation;
    };

    const QFileInfo coreInfo(corePath);
    if (!coreInfo.exists() || !coreInfo.isFile()) {
        const QString reason = coreInfo.exists()
            ? QStringLiteral("请选择文件，而不是文件夹。")
            : QStringLiteral("找不到这个文件：%1").arg(corePath);
        fileStep.situation = QStringLiteral("未执行：%1").arg(reason);
        notesStep.situation = QStringLiteral("未执行：文件不可用");
        unstripStep.situation = QStringLiteral("未执行：文件不可用");
        gdbStep.situation = QStringLiteral("未执行：文件不可用");
        fileStep.meaning = QStringLiteral("文件不存在或不是普通文件时，不会运行分析命令。");
        outcome.steps = {fileStep, notesStep, unstripStep, gdbStep};
        const QString fault = coreInfo.exists()
            ? QStringLiteral("选中的路径不是 Linux core 文件。%1").arg(reason)
            : reason;
        fail(fault, QStringLiteral("没有可读的 core 文件，所以无法判断程序为何停止。"),
             {QStringLiteral("请重新选择一个存在的 core 文件。")});
        return outcome;
    }

    QString headerDetail;
    const FileKind kind = classifyFile(corePath, &headerDetail);
    outcome.coreFile = kind == FileKind::Core;

    const ArgvResult fileResult = runPlan(filePlan, &fileStep);
    if (fileBin.isEmpty())
        outcome.caveats << QStringLiteral("本机没有找到 file，文件类型改由文件头判断。");
    else if (fileResult.state == ArgvState::StartFailed || fileResult.state == ArgvState::TimedOut)
        outcome.caveats << QStringLiteral("file 没有完成：%1").arg(fileStep.situation);
    if (!fileStep.excerpt.isEmpty())
        absorbFileText(fileStep.excerpt, &facts);
    fileStep.meaning = fileMeaning(fileStep.excerpt, outcome.coreFile);
    outcome.steps.append(fileStep);
    if (outcome.stoppedEarly) {
        cancelRest(&notesStep, &unstripStep, &gdbStep);
        outcome.steps << notesStep << unstripStep << gdbStep;
        fail(QStringLiteral("分析已取消。已经做完的步骤保留在下面。"),
             QStringLiteral("分析被中途取消。"), {QStringLiteral("需要的话可以重新开始。")});
        outcome.stoppedEarly = true;
        return outcome;
    }

    if (!outcome.coreFile) {
        const QString skipped = QStringLiteral("未执行：文件不是 core 记录");
        notesStep.situation = skipped;
        unstripStep.situation = skipped;
        gdbStep.situation = skipped;
        notesStep.meaning = QStringLiteral("只有 core 记录才含有程序停止时的信号和步骤。");
        outcome.steps << notesStep << unstripStep << gdbStep;
        const QString fault = headerDetail.isEmpty()
            ? QStringLiteral("选中的文件不是 Linux core 文件，不能当作崩溃现场来分析。")
            : QStringLiteral("选中的文件不是 Linux core 文件。%1").arg(headerDetail);
        fail(fault, QStringLiteral("文件内容不是程序异常退出时留下的现场记录。"),
             {QStringLiteral("请重新选择 core 文件。它一般在程序意外关闭时由系统生成，名字里常带 core。"),
              QStringLiteral("如果选中的是程序本身，请把它填到“程序文件”，core 文件另选。")});
        return outcome;
    }

    if (euReadelfMissing && notesBin.isEmpty()) {
        outcome.caveats << QStringLiteral("本机没有找到 eu-readelf，也没有找到 readelf。停止信号改由 gdb 读取。");
    } else if (euReadelfMissing) {
        outcome.caveats << QStringLiteral("本机没有找到 eu-readelf，已改用 readelf。");
    }

    const ArgvResult notesResult = runPlan(notesPlan, &notesStep);
    if (notesResult.state == ArgvState::StartFailed || notesResult.state == ArgvState::TimedOut)
        outcome.caveats << QStringLiteral("读取停止信号的命令没有完成：%1").arg(notesStep.situation);
    if (!notesStep.excerpt.isEmpty())
        absorbNotes(notesStep.excerpt, &facts);
    reconcileSignal(&facts);
    notesStep.meaning = notesMeaning(notesStep.excerpt, facts);
    outcome.steps.append(notesStep);
    if (outcome.stoppedEarly) {
        cancelRest(&unstripStep, &gdbStep, nullptr);
        outcome.steps << unstripStep << gdbStep;
        fail(QStringLiteral("分析已取消。已经做完的步骤保留在下面。"),
             QStringLiteral("分析被中途取消。"), {QStringLiteral("需要的话可以重新开始。")});
        outcome.stoppedEarly = true;
        return outcome;
    }

    const ArgvResult unstripResult = runPlan(unstripPlan, &unstripStep);
    if (unstripBin.isEmpty())
        outcome.caveats << QStringLiteral("本机没有找到 eu-unstrip。没有它也可以继续，只是少一项对照。");
    else if (unstripResult.state == ArgvState::StartFailed || unstripResult.state == ArgvState::TimedOut)
        outcome.caveats << QStringLiteral("eu-unstrip 没有完成：%1").arg(unstripStep.situation);
    unstripStep.meaning = unstripMeaning(unstripStep.excerpt);
    outcome.steps.append(unstripStep);
    if (outcome.stoppedEarly) {
        cancelRest(&gdbStep, nullptr, nullptr);
        outcome.steps << gdbStep;
        fail(QStringLiteral("分析已取消。已经做完的步骤保留在下面。"),
             QStringLiteral("分析被中途取消。"), {QStringLiteral("需要的话可以重新开始。")});
        outcome.stoppedEarly = true;
        return outcome;
    }

    const bool gdbMissing = gdbBin.isEmpty();
    const ArgvResult gdbResult = runPlan(gdbPlan, &gdbStep);
    if (gdbMissing)
        outcome.caveats << QStringLiteral("本机没有找到 gdb，所以看不到程序停在哪一步。");
    else if (gdbResult.state == ArgvState::StartFailed || gdbResult.state == ArgvState::TimedOut)
        outcome.caveats << QStringLiteral("调试器没有完成：%1").arg(gdbStep.situation);
    if (!gdbStep.excerpt.isEmpty())
        absorbDebugger(gdbStep.excerpt, &facts);
    reconcileSignal(&facts);
    gdbStep.meaning = gdbMissing
        ? QStringLiteral("本机没有找到 gdb。安装后再分析，才能看到停在哪一步。")
        : debuggerMeaning(facts);
    outcome.steps.append(gdbStep);
    if (outcome.stoppedEarly) {
        fail(QStringLiteral("分析已取消。已经做完的步骤保留在下面。"),
             QStringLiteral("分析被中途取消。"), {QStringLiteral("需要的话可以重新开始。")});
        outcome.stoppedEarly = true;
        return outcome;
    }

    reconcileSignal(&facts);
    outcome.signalCode = facts.signalCode;
    outcome.signalToken = facts.signalToken;
    outcome.place = placeText(facts);
    if (facts.frames.isEmpty() && gdbMissing)
        outcome.place += QStringLiteral("本机没有找到 gdb，所以看不到停在哪一步。");
    outcome.fault = faultText(facts);
    outcome.likely = likelyText(facts);
    outcome.advice = numberLines(adviceLines(facts, gdbMissing));
    outcome.accepted = facts.signalCode > 0 || !facts.signalToken.isEmpty() || !facts.frames.isEmpty();
    if (!outcome.accepted) {
        outcome.fault = QStringLiteral("这是一份 core 文件，但这次没能读出程序为何停止。");
        outcome.likely = QStringLiteral("现场记录已确认，可是读取信号的命令没有给出可用结果。");
        if (outcome.advice.isEmpty()) {
            outcome.advice = numberLines(
                {QStringLiteral("请确认已安装 gdb 或 eu-readelf，然后重新分析。"),
                 QStringLiteral("请先不要删除这份 core 文件，后面排查还要用。")});
        }
    }
    publish();
    return outcome;
}

} // namespace tw
