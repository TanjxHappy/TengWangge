#include "tools/core_analyze/core_analyzer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

namespace tw {
namespace {

constexpr int kReportOutputChars = 4000;

struct SignalPlain
{
    int number;
    const char *name;
    const char *title;
    const char *cause;
    const char *advice;
};

const SignalPlain kSignals[] = {
    {4, "SIGILL", "碰到了这台电脑不认识的指令",
     "程序文件可能和这台电脑不匹配，或者文件已经损坏。",
     "请确认安装的版本适合这台电脑，必要时重新安装后再试。"},
    {5, "SIGTRAP", "停在了调试断点上",
     "程序被调试断点拦住了。这通常不是日常使用时会出现的情况。",
     "请把这份说明交给负责该软件的同事。"},
    {6, "SIGABRT", "自己停了下来",
     "程序发现没法继续，于是主动中止了。",
     "请回忆停止前有没有弹出提示，把提示内容和这份说明一起交给负责该软件的同事。"},
    {7, "SIGBUS", "读到了无效的存储位置",
     "程序访问的存储位置无效，有时是相关文件不完整。",
     "请确认相关文件没有被占用或写到一半，换一份完整文件后再试。"},
    {8, "SIGFPE", "做了不允许的运算",
     "程序做了不能完成的计算，例如把数字除以零。",
     "请检查刚才填写的数字是不是 0，或是不是超出了正常范围，换一组数值后再试。"},
    {11, "SIGSEGV", "访问了不允许使用的内存",
     "程序去读或写一块现在不能用的内存。",
     "请记下停止前的最后一步操作，并把这份说明交给负责该软件的同事。"},
    {13, "SIGPIPE", "向已经断开的连接写了数据",
     "对方已经断开，程序还在继续发送内容。",
     "请确认网络或对方程序仍在运行，重新连接后再试。"},
    {24, "SIGXCPU", "占用处理时间过长，被系统停下",
     "程序运行的时间超过了系统允许的上限。",
     "请稍后再试。如果经常出现，请把这份说明交给负责该软件的同事。"},
    {25, "SIGXFSZ", "写出的文件超过了允许的大小",
     "程序想写入的内容超过了系统限制。",
     "请清理磁盘空间，或请管理员放宽文件大小限制后再试。"},
    {31, "SIGSYS", "使用了系统不支持的操作",
     "程序向系统发起了一个当前系统不认识的请求。",
     "请确认软件版本和系统匹配，并把这份说明交给负责该软件的同事。"},
};

struct StackFrame
{
    QString address;
    QString function;
    QString location;
};

struct CrashFacts
{
    int signalNumber = 0;
    QString signalName;
    QString signalDescription;
    QString faultAddress;
    QString programHint;
    QVector<StackFrame> frames;
    int threadCount = 0;
    bool sawAbort = false;
    bool sawStackCheck = false;
    bool sawAllocator = false;
    bool symbolsMissing = false;
    bool executableMismatch = false;
};

QString quoteArg(const QString &arg)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_./:+@%=,-]+$"));
    if (!arg.isEmpty() && safe.match(arg).hasMatch())
        return arg;
    QString escaped = arg;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'%1'").arg(escaped);
}

QString absolutePath(const QString &path)
{
    if (path.isEmpty())
        return path;
    return QFileInfo(path).absoluteFilePath();
}

bool hasControlChar(const QString &path)
{
    for (const QChar ch : path) {
        if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r') || ch.unicode() < 0x20)
            return true;
    }
    return false;
}

const SignalPlain *signalByNumber(int number)
{
    for (const SignalPlain &item : kSignals) {
        if (item.number == number)
            return &item;
    }
    return nullptr;
}

const SignalPlain *signalByName(const QString &name)
{
    for (const SignalPlain &item : kSignals) {
        if (name == QLatin1String(item.name))
            return &item;
    }
    return nullptr;
}

QString defaultFind(const QString &name)
{
    return QStandardPaths::findExecutable(name);
}

enum class HeaderKind
{
    Core,
    OtherElf,
    NotElf,
    TooShort,
    Unreadable
};

HeaderKind readHeader(const QString &path, QString *detail)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *detail = QStringLiteral("文件打不开：%1").arg(file.errorString());
        return HeaderKind::Unreadable;
    }
    const QByteArray header = file.read(64);
    if (header.size() < 20) {
        *detail = QStringLiteral("文件太短，不是 core 记录。");
        return HeaderKind::TooShort;
    }
    const auto *bytes = reinterpret_cast<const uchar *>(header.constData());
    if (!(bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F')) {
        *detail = QStringLiteral("文件开头不是 ELF，通常是普通文档、图片或压缩包。");
        return HeaderKind::NotElf;
    }
    const int elfClass = bytes[4];
    const int endian = bytes[5];
    if ((elfClass != 1 && elfClass != 2) || (endian != 1 && endian != 2)) {
        *detail = QStringLiteral("文件像是 ELF，但头部不完整。");
        return HeaderKind::NotElf;
    }
    const auto read16 = [&](int offset) {
        if (endian == 1)
            return quint16(bytes[offset]) | (quint16(bytes[offset + 1]) << 8);
        return (quint16(bytes[offset]) << 8) | quint16(bytes[offset + 1]);
    };
    const quint16 type = read16(16);
    if (type == 4)
        return HeaderKind::Core;
    if (type == 2 || type == 3) {
        *detail = QStringLiteral("这是一个程序文件，不是 core 记录。程序文件可以填在“程序文件”一栏。");
        return HeaderKind::OtherElf;
    }
    *detail = QStringLiteral("这是 ELF 文件，但不是 core 记录。");
    return HeaderKind::OtherElf;
}

QString gdbQuote(const QString &path)
{
    QString escaped;
    escaped.reserve(path.size());
    for (const QChar ch : path) {
        if (ch == QLatin1Char('\\') || ch == QLatin1Char('"'))
            escaped += QLatin1Char('\\');
        escaped += ch;
    }
    return QStringLiteral("\"%1\"").arg(escaped);
}

QString clipOutput(const QString &output, bool truncated)
{
    QString text = output.trimmed();
    if (text.size() > kReportOutputChars) {
        text.truncate(kReportOutputChars);
        truncated = true;
    }
    if (truncated)
        text += QStringLiteral("\n…（输出过长，已截断，只保留前面这部分）");
    return text;
}

QString statusFor(const CommandResult &result)
{
    switch (result.status) {
    case CommandStatus::Ran:
        return result.exitCode == 0
            ? QStringLiteral("已执行")
            : QStringLiteral("已执行（命令返回码 %1）").arg(result.exitCode);
    case CommandStatus::TimedOut:
        return QStringLiteral("已中止：%1").arg(result.errorDetail);
    case CommandStatus::FailedToStart:
        return QStringLiteral("没有跑起来：%1").arg(result.errorDetail);
    case CommandStatus::Cancelled:
        return QStringLiteral("已取消");
    case CommandStatus::MissingProgram:
        return QStringLiteral("未执行");
    }
    return QStringLiteral("未执行");
}

void noteProgram(CrashFacts *facts, const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
        return;
    if (facts->programHint.isEmpty()) {
        facts->programHint = trimmed;
        return;
    }
    const bool havePath = facts->programHint.contains(QLatin1Char('/'));
    const bool newPath = trimmed.contains(QLatin1Char('/'));
    if (!havePath && newPath)
        facts->programHint = trimmed;
}

void absorbFileOutput(const QString &text, CrashFacts *facts)
{
    const auto take = [&](const QString &pattern) {
        const QRegularExpression re(pattern);
        const QRegularExpressionMatch match = re.match(text);
        if (match.hasMatch())
            noteProgram(facts, match.captured(1));
    };
    take(QStringLiteral("execfn:\\s*'([^']*)'"));
    take(QStringLiteral("from\\s+'([^']*)'"));
}

void absorbReadelf(const QString &text, CrashFacts *facts)
{
    int signo = 0;
    const QRegularExpression signoRe(QStringLiteral("\\bsi_signo:\\s*(\\d+)"));
    const QRegularExpression cursigRe(QStringLiteral("\\bcursig:\\s*(\\d+)"));
    QRegularExpressionMatchIterator signoIt = signoRe.globalMatch(text);
    while (signoIt.hasNext()) {
        const int value = signoIt.next().captured(1).toInt();
        if (value > 0) {
            signo = value;
            break;
        }
    }
    if (signo == 0) {
        QRegularExpressionMatchIterator cursigIt = cursigRe.globalMatch(text);
        while (cursigIt.hasNext()) {
            const int value = cursigIt.next().captured(1).toInt();
            if (value > 0) {
                signo = value;
                break;
            }
        }
    }
    if (signo > 0 && facts->signalNumber == 0)
        facts->signalNumber = signo;

    const QRegularExpression addrRe(QStringLiteral("\\bsi_addr:\\s*(\\S+)"));
    const QRegularExpressionMatch addr = addrRe.match(text);
    if (addr.hasMatch() && facts->faultAddress.isEmpty())
        facts->faultAddress = addr.captured(1);

    const QRegularExpression psargs(QStringLiteral("\\bpsargs:\\s*(.+)"));
    const QRegularExpression fname(QStringLiteral("\\bfname:\\s*(\\S+)"));
    const QRegularExpressionMatch ps = psargs.match(text);
    if (ps.hasMatch())
        noteProgram(facts, ps.captured(1));
    const QRegularExpressionMatch fn = fname.match(text);
    if (fn.hasMatch())
        noteProgram(facts, fn.captured(1));
}

bool isNullAddress(const QString &address)
{
    const QString value = address.trimmed().toLower();
    return value == QLatin1String("0")
        || value == QLatin1String("0x0")
        || value == QLatin1String("(nil)")
        || value == QLatin1String("nil")
        || value == QLatin1String("0x0000000000000000");
}

bool unknownFunction(const QString &function)
{
    return function.isEmpty() || function == QLatin1String("??");
}

void splitFrameRest(const QString &rest, QString *function, QString *location)
{
    const int atPos = rest.lastIndexOf(QStringLiteral(" at "));
    const int fromPos = rest.lastIndexOf(QStringLiteral(" from "));
    int cut = -1;
    QString loc;
    if (atPos >= 0 && (fromPos < 0 || atPos > fromPos)) {
        cut = atPos;
        loc = rest.mid(atPos + 4).trimmed();
    } else if (fromPos >= 0) {
        cut = fromPos;
        loc = rest.mid(fromPos + 6).trimmed();
    }
    QString func = (cut >= 0 ? rest.left(cut) : rest).trimmed();
    const int paren = func.lastIndexOf(QStringLiteral(" ("));
    if (paren > 0)
        func = func.left(paren).trimmed();
    *function = func;
    *location = loc;
}

StackFrame parseFrameLine(const QString &line)
{
    StackFrame frame;
    static const QRegularExpression withAddr(
        QStringLiteral("^#\\d+\\s+(0x[0-9a-fA-F]+)\\s+in\\s+(.*)$"));
    static const QRegularExpression bare(QStringLiteral("^#\\d+\\s+(\\S.*)$"));
    const QRegularExpressionMatch addressed = withAddr.match(line);
    if (addressed.hasMatch()) {
        frame.address = addressed.captured(1);
        splitFrameRest(addressed.captured(2), &frame.function, &frame.location);
        return frame;
    }
    const QRegularExpressionMatch plain = bare.match(line);
    if (plain.hasMatch())
        splitFrameRest(plain.captured(1), &frame.function, &frame.location);
    return frame;
}

void absorbGdb(const QString &text, CrashFacts *facts)
{
    const QRegularExpression terminated(
        QStringLiteral("terminated with signal\\s+(SIG[A-Z0-9]+|\\d+)\\s*,\\s*([^\\r\\n]+)"));
    const QRegularExpressionMatch term = terminated.match(text);
    if (term.hasMatch()) {
        const QString token = term.captured(1);
        QString description = term.captured(2).trimmed();
        if (description.endsWith(QLatin1Char('.')))
            description.chop(1);
        facts->signalDescription = description;
        if (token.startsWith(QLatin1String("SIG")))
            facts->signalName = token;
        else
            facts->signalNumber = token.toInt();
    }
    if (facts->signalName.isEmpty()) {
        const QRegularExpression named(QStringLiteral("\\b(SIG[A-Z][A-Z0-9]+)\\b"));
        const QRegularExpressionMatch match = named.match(text);
        if (match.hasMatch())
            facts->signalName = match.captured(1);
    }

    const QRegularExpression generated(QStringLiteral("Core was generated by `([^`]*)'"));
    const QRegularExpressionMatch gen = generated.match(text);
    if (gen.hasMatch())
        noteProgram(facts, gen.captured(1));

    if (text.contains(QLatin1String("core file may not match"), Qt::CaseInsensitive)
        || text.contains(QLatin1String("exec file is newer"), Qt::CaseInsensitive)) {
        facts->executableMismatch = true;
    }
    if (text.contains(QLatin1String("No symbol table"), Qt::CaseInsensitive)
        || text.contains(QLatin1String("no debugging symbols"), Qt::CaseInsensitive)) {
        facts->symbolsMissing = true;
    }

    const QStringList lines = text.split(QLatin1Char('\n'));
    QVector<QVector<StackFrame>> chunks;
    QVector<StackFrame> current;
    int threads = 0;
    auto flush = [&]() {
        if (!current.isEmpty())
            chunks.append(current);
        current.clear();
    };
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("Thread "))) {
            ++threads;
            flush();
            continue;
        }
        if (!line.startsWith(QLatin1Char('#')))
            continue;
        const StackFrame frame = parseFrameLine(line);
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

    auto mentions = [](const QString &function, const QStringList &needles) {
        for (const QString &needle : needles) {
            if (function == needle || function.endsWith(QLatin1Char('_') + needle)
                || function.endsWith(QStringLiteral("::") + needle)) {
                return true;
            }
        }
        return false;
    };
    for (const StackFrame &frame : facts->frames) {
        if (mentions(frame.function, {QStringLiteral("abort"), QStringLiteral("raise")}))
            facts->sawAbort = true;
        if (frame.function.contains(QLatin1String("__stack_chk_fail"))
            || frame.function.contains(QLatin1String("stack_chk_fail"))) {
            facts->sawStackCheck = true;
        }
        if (mentions(frame.function, {QStringLiteral("malloc"), QStringLiteral("free")})
            || frame.function.contains(QLatin1String("operator delete"))
            || frame.function.contains(QLatin1String("operator new"))) {
            facts->sawAllocator = true;
        }
    }
    if (!facts->frames.isEmpty() && unknownFunction(facts->frames.first().function))
        facts->symbolsMissing = true;
}

void finishSignalNames(CrashFacts *facts)
{
    if (!facts->signalName.isEmpty() && facts->signalNumber == 0) {
        if (const SignalPlain *item = signalByName(facts->signalName))
            facts->signalNumber = item->number;
    }
    if (facts->signalName.isEmpty() && facts->signalNumber > 0) {
        if (const SignalPlain *item = signalByNumber(facts->signalNumber))
            facts->signalName = QLatin1String(item->name);
    }
}

QString whereText(const CrashFacts &facts)
{
    QStringList parts;
    if (!facts.programHint.isEmpty())
        parts << QStringLiteral("当时运行的程序是“%1”。").arg(facts.programHint);
    if (facts.threadCount > 1) {
        parts << QStringLiteral("这份记录里有 %1 条同时在做的事，下面说的是出问题的那一条。")
                     .arg(facts.threadCount);
    }
    if (facts.frames.isEmpty()) {
        parts << QStringLiteral("这次没有得到程序停下前走过的步骤。");
        return parts.join(QString());
    }
    const StackFrame &top = facts.frames.first();
    if (unknownFunction(top.function)) {
        if (top.address.isEmpty())
            parts << QStringLiteral("程序已经停下，但当前看不出停在哪一个步骤。");
        else
            parts << QStringLiteral("程序停在地址 %1，当前看不出步骤名称。").arg(top.address);
    } else {
        parts << QStringLiteral("程序停在“%1”这一步。").arg(top.function);
        if (!top.location.isEmpty())
            parts << QStringLiteral("对应位置：%1。").arg(top.location);
    }
    QStringList earlier;
    const int limit = std::min(static_cast<int>(facts.frames.size()), 4);
    for (int i = 1; i < limit; ++i) {
        const QString &name = facts.frames.at(i).function;
        earlier << (unknownFunction(name) ? QStringLiteral("未知步骤") : name);
    }
    if (!earlier.isEmpty())
        parts << QStringLiteral("再往前经过：%1。").arg(earlier.join(QStringLiteral(" → ")));
    return parts.join(QString());
}

QString errorText(const CrashFacts &facts)
{
    const SignalPlain *item = signalByName(facts.signalName);
    if (!item && facts.signalNumber > 0)
        item = signalByNumber(facts.signalNumber);
    if (!item && !facts.frames.isEmpty()) {
        return QStringLiteral("程序已经停止。这次没有读出系统代号，停下的位置写在下面。");
    }
    if (item) {
        QString text = QStringLiteral("程序因为“%1”而停止").arg(QString::fromUtf8(item->title));
        if (!facts.signalName.isEmpty())
            text += QStringLiteral("（系统代号 %1，编号 %2）").arg(facts.signalName).arg(facts.signalNumber);
        else
            text += QStringLiteral("（编号 %1）").arg(facts.signalNumber);
        text += QStringLiteral("。");
        return text;
    }
    if (!facts.signalName.isEmpty())
        return QStringLiteral("程序因为系统信号 %1 而停止。").arg(facts.signalName);
    if (facts.signalNumber > 0)
        return QStringLiteral("程序因为系统信号 %1 而停止。").arg(facts.signalNumber);
    return QStringLiteral("这是一份 core 文件，但这次没能读出程序为何停止。");
}

QString causeText(const CrashFacts &facts)
{
    QStringList parts;
    const SignalPlain *item = signalByName(facts.signalName);
    if (!item && facts.signalNumber > 0)
        item = signalByNumber(facts.signalNumber);
    if (item)
        parts << QString::fromUtf8(item->cause);
    else
        parts << QStringLiteral("这次只能确认程序异常停止，具体原因需要对照下方原始输出。");

    if (facts.signalNumber == 11 && isNullAddress(facts.faultAddress)) {
        parts << QStringLiteral("它用到的位置是空的（地址是 0），常见情况是数据还没准备好就被使用了。");
    }
    if (facts.sawStackCheck)
        parts << QStringLiteral("程序写入的内容超出了预留空间，把旁边的数据冲坏了。");
    if (facts.sawAllocator) {
        parts << QStringLiteral("问题出在整理内存的时候，常见情况是同一份数据被放弃了两次，或还在使用已经放弃的数据。");
    }
    if (facts.sawAbort && facts.signalNumber != 6)
        parts << QStringLiteral("调用过程里出现了程序自己要求停止的步骤。");
    if (facts.executableMismatch) {
        parts << QStringLiteral("填入的程序文件和这份记录对不上，步骤名称可能不准确。");
    }
    if (facts.symbolsMissing) {
        parts << QStringLiteral("有些步骤只剩地址、没有名称。若手边有调试符号或未裁剪的程序文件，填上后可以重新分析。");
    }
    return parts.join(QString());
}

QStringList suggestionList(const CrashFacts &facts, bool gdbMissing)
{
    QStringList items;
    const SignalPlain *item = signalByName(facts.signalName);
    if (!item && facts.signalNumber > 0)
        item = signalByNumber(facts.signalNumber);
    if (item)
        items << QString::fromUtf8(item->advice);
    else
        items << QStringLiteral("请把导出的说明交给负责该软件的同事。");
    items << QStringLiteral("请先不要删除这份 core 文件，排查时还需要它。");
    if (facts.symbolsMissing) {
        items << QStringLiteral("如果手边有调试符号或未裁剪的程序文件，填到上方后重新分析，步骤名称会更完整。");
    }
    if (gdbMissing)
        items << QStringLiteral("请安装 gdb 后重新分析，才能看到程序停在哪一个步骤。");
    return items;
}

QString numberSuggestions(const QStringList &items)
{
    QStringList lines;
    for (int i = 0; i < items.size(); ++i)
        lines << QStringLiteral("%1. %2").arg(i + 1).arg(items.at(i));
    return lines.join(QLatin1Char('\n'));
}

QString fileInterpretation(const QString &output, bool isCore)
{
    const bool saysCore = output.contains(QLatin1String("core file"), Qt::CaseInsensitive)
        || output.contains(QLatin1String("core dump"), Qt::CaseInsensitive)
        || output.contains(QStringLiteral("核心转储"));
    if (saysCore) {
        return QStringLiteral("输出里出现 core file（或“核心转储”），表示这是程序异常退出时留下的现场记录，可以继续看它为什么停止。");
    }
    if (isCore) {
        return QStringLiteral("文件头已经确认这是 core 记录。上面是系统对文件类型的描述，用词可能和“core file”不完全一样。");
    }
    return QStringLiteral("输出里没有 core file，说明它不是崩溃现场记录。分析停在这一步，不会再对它运行调试器。");
}

QString readelfInterpretation(const QString &output, const CrashFacts &facts)
{
    if (output.contains(QLatin1String("si_signo")) || output.contains(QLatin1String("cursig"))) {
        QString text = QStringLiteral("si_signo 或 cursig 后面的数字是停止代号。");
        if (facts.signalNumber > 0) {
            text += QStringLiteral("这次读到的是 %1").arg(facts.signalNumber);
            if (!facts.signalName.isEmpty())
                text += QStringLiteral("，也就是 %1").arg(facts.signalName);
            text += QStringLiteral("。");
        }
        if (isNullAddress(facts.faultAddress))
            text += QStringLiteral(" si_addr 为 0，表示用到了空的位置。");
        else if (!facts.faultAddress.isEmpty())
            text += QStringLiteral(" si_addr 是出问题的位置。");
        return text;
    }
    if (output.contains(QLatin1String("Cannot decode")) || output.contains(QLatin1String("description data"))) {
        return QStringLiteral("这条命令没有把记录翻译成可读文字（有的 readelf 打不开 64 位记录）。停止原因改看后面的调试器输出。");
    }
    return QStringLiteral("这次没有从记录里读出停止代号。若后面的调试器有输出，以调试器为准。");
}

QString unstripInterpretation()
{
    return QStringLiteral("这个命令用来看程序和它的对照信息是否齐全。缺少对照信息时，仍能判断停止原因，但步骤名称可能只显示为问号。");
}

QString gdbInterpretation(const CrashFacts &facts)
{
    QString text = QStringLiteral("“Program terminated with signal”或“信号 SIG…”表示程序因该信号停止。以 #0 开头的一行是停下的位置，下面几行是更早的步骤。问号表示当前没有步骤名称。");
    if (facts.executableMismatch)
        text += QStringLiteral(" 输出里还提到程序文件和记录对不上，请换用生成这份记录时的那个程序文件。");
    return text;
}

QString renderReport(const AnalysisReport &report)
{
    QString text;
    text += QStringLiteral("文件分析说明\n");
    text += QStringLiteral("生成时间：%1\n").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    text += QStringLiteral("原始命令输出保持英文，是为了和下面的中文释义一一对应。\n\n");

    text += QStringLiteral("一、结论\n");
    text += QStringLiteral("报错信息：%1\n").arg(report.errorInfo.isEmpty() ? QStringLiteral("无") : report.errorInfo);
    text += QStringLiteral("所在步骤：%1\n").arg(report.whereInfo.isEmpty() ? QStringLiteral("无") : report.whereInfo);
    text += QStringLiteral("可能原因：%1\n").arg(report.cause.isEmpty() ? QStringLiteral("无") : report.cause);
    text += QStringLiteral("建议解决方案：\n");
    text += report.suggestionText.isEmpty() ? QStringLiteral("无\n") : report.suggestionText + QLatin1Char('\n');
    text += QLatin1Char('\n');

    text += QStringLiteral("二、如何查看和分析 core 文件\n");
    text += QStringLiteral("core 文件是程序异常退出时，操作系统留下的一份现场记录。\n");
    text += QStringLiteral("不需要先学会调试。按下面的顺序看：每一步都写了要运行的命令、这次实际跑过没有、关键输出，以及这些输出在说什么。\n\n");

    if (report.steps.isEmpty()) {
        text += CoreAnalyzer::usageGuide();
        text += QLatin1Char('\n');
    }
    for (const ExecutedStep &step : report.steps) {
        text += step.title + QLatin1Char('\n');
        text += QStringLiteral("这一步做什么：%1\n").arg(step.purpose);
        text += QStringLiteral("命令：\n    %1\n").arg(step.commandDisplay);
        text += QStringLiteral("本次执行：%1\n").arg(step.statusText);
        if (!step.output.isEmpty())
            text += QStringLiteral("关键输出：\n%1\n").arg(step.output);
        if (!step.interpretation.isEmpty())
            text += QStringLiteral("释义：%1\n").arg(step.interpretation);
        text += QLatin1Char('\n');
    }

    text += QStringLiteral("三、还需要知道的限制\n");
    if (report.limits.isEmpty())
        text += QStringLiteral("没有额外限制。本次用到的命令都已写在上面。\n");
    else {
        for (const QString &limit : report.limits)
            text += QStringLiteral("- %1\n").arg(limit);
    }
    return text;
}

struct ToolBins
{
    QString file;
    QString readelf;
    QString readelfName;
    QString unstrip;
    QString gdb;
};

ToolBins resolveTools(const ToolFinder &finder)
{
    ToolBins bins;
    bins.file = finder(QStringLiteral("file"));
    bins.readelf = finder(QStringLiteral("eu-readelf"));
    bins.readelfName = QStringLiteral("eu-readelf");
    if (bins.readelf.isEmpty()) {
        bins.readelf = finder(QStringLiteral("readelf"));
        bins.readelfName = QStringLiteral("readelf");
    }
    bins.unstrip = finder(QStringLiteral("eu-unstrip"));
    bins.gdb = finder(QStringLiteral("gdb"));
    return bins;
}

} // namespace

QString CoreAnalyzer::usageGuide()
{
    return QStringLiteral(
        "如何查看和分析 core 文件\n"
        "\n"
        "core 文件是程序异常退出时，操作系统留下的一份现场记录。可以按下面四步看懂大致原因。\n"
        "\n"
        "步骤 1  确认文件类型\n"
        "命令：file -b -- <core 文件>\n"
        "释义：输出里出现 core file，才是崩溃现场记录。普通文档或图片不会出现这几个词。\n"
        "\n"
        "步骤 2  查看停止信号\n"
        "命令：eu-readelf -n -W -- <core 文件>\n"
        "若没有 eu-readelf，可以改用 readelf -n -W -- <core 文件>。\n"
        "释义：si_signo 或 cursig 后面的数字是停止代号。11 表示访问了不允许使用的内存，6 表示程序自己停了下来。\n"
        "\n"
        "步骤 3  核对调试信息\n"
        "命令：eu-unstrip -n --core <core 文件>\n"
        "释义：用来看程序有没有配套的对照信息。没有它也能知道停止原因，只是步骤名称可能看不到。\n"
        "\n"
        "步骤 4  查看停下的步骤\n"
        "命令：gdb -batch -nx -q -iex \"set pagination off\" -iex \"set confirm off\" <程序文件> <core 文件> -ex \"thread apply all bt\"\n"
        "没有程序文件时，把 <程序文件> 换成 -c <core 文件>。调试符号若是一个目录，在前面加上 -iex \"set debug-file-directory <目录>\"。\n"
        "释义：Program terminated with signal 后面是停止原因。以 #0 开头的一行是停下的位置。\n"
        "\n"
        "选好文件后点击“开始分析”，上面的命令会实际执行，并在每条命令下附上本次输出和中文释义。\n");
}

QString CoreAnalyzer::formatCommand(const QString &program, const QStringList &arguments)
{
    QStringList parts;
    parts << quoteArg(program);
    for (const QString &argument : arguments)
        parts << quoteArg(argument);
    return parts.join(QLatin1Char(' '));
}

AnalysisReport CoreAnalyzer::analyze(const AnalysisRequest &request,
                                     CommandExecutor &executor,
                                     const ToolFinder &finder,
                                     const std::atomic<bool> *cancel,
                                     const std::function<void(const QString &)> &progress)
{
    const ToolFinder find = finder ? finder : ToolFinder(defaultFind);
    const auto cancelled = [&]() { return cancel && cancel->load(); };

    AnalysisReport report;
    CrashFacts facts;
    const ToolBins bins = resolveTools(find);

    const auto fail = [&](const QString &error, const QString &cause, const QStringList &suggestions) {
        report.ok = false;
        report.errorInfo = error;
        report.cause = cause;
        report.suggestionText = numberSuggestions(suggestions);
        if (report.whereInfo.isEmpty())
            report.whereInfo = QStringLiteral("没有继续查看程序停下的位置。");
        report.rendered = renderReport(report);
    };

    if (cancelled()) {
        report.cancelled = true;
        fail(QStringLiteral("分析已取消。"),
             QStringLiteral("分析在开始前被取消了。"),
             {QStringLiteral("需要的话可以重新开始。")});
        return report;
    }

    const QString coreInput = request.corePath.trimmed();
    if (coreInput.isEmpty()) {
        fail(QStringLiteral("请先选择一个 core 文件。"),
             QStringLiteral("还没有选择要分析的文件。"),
             {QStringLiteral("点击“浏览”，选择程序异常退出时留下的 core 文件。")});
        report.steps.clear();
        report.rendered = renderReport(report);
        return report;
    }
    if (hasControlChar(coreInput) || hasControlChar(request.executablePath) || hasControlChar(request.symbolPath)) {
        fail(QStringLiteral("路径里不能包含换行或控制字符。"),
             QStringLiteral("这样的路径无法安全地传给分析命令。"),
             {QStringLiteral("请换成普通文件路径后再试。")});
        return report;
    }

    const QString corePath = absolutePath(coreInput);
    QString exePath = request.executablePath.trimmed().isEmpty() ? QString() : absolutePath(request.executablePath.trimmed());
    QString symbolPath = request.symbolPath.trimmed().isEmpty() ? QString() : absolutePath(request.symbolPath.trimmed());

    QStringList limits;
    if (!exePath.isEmpty() && !QFileInfo(exePath).isFile()) {
        limits << QStringLiteral("填写的程序文件找不到，已改为只根据 core 文件分析：%1").arg(exePath);
        exePath.clear();
    }
    bool symbolIsDir = false;
    if (!symbolPath.isEmpty()) {
        const QFileInfo symbolInfo(symbolPath);
        if (symbolInfo.isDir()) {
            symbolIsDir = true;
        } else if (!symbolInfo.isFile()) {
            limits << QStringLiteral("填写的调试符号找不到，已忽略：%1").arg(symbolPath);
            symbolPath.clear();
        }
    }

    auto commandFor = [&](const QString &program, const QString &fallback, const QStringList &arguments) {
        return formatCommand(program.isEmpty() ? fallback : program, arguments);
    };

    auto runOne = [&](const QString &program, const QStringList &arguments, int timeoutMs, ExecutedStep *step) -> CommandResult {
        CommandRequest req;
        req.program = program;
        req.arguments = arguments;
        req.workDirectory = request.workDirectory;
        req.timeoutMs = timeoutMs;
        if (cancelled()) {
            CommandResult cancelledResult;
            cancelledResult.status = CommandStatus::Cancelled;
            step->statusText = QStringLiteral("已取消");
            report.cancelled = true;
            return cancelledResult;
        }
        if (program.isEmpty()) {
            CommandResult missing;
            missing.status = CommandStatus::MissingProgram;
            step->statusText = QStringLiteral("未执行：系统里没有找到这个命令");
            return missing;
        }
        if (progress)
            progress(step->title);
        const CommandResult result = executor.run(req);
        step->ran = result.status == CommandStatus::Ran || result.status == CommandStatus::TimedOut;
        step->statusText = statusFor(result);
        step->output = clipOutput(result.output, result.truncated);
        if (result.status == CommandStatus::Cancelled)
            report.cancelled = true;
        return result;
    };

    const QStringList fileArgs = {QStringLiteral("-b"), QStringLiteral("--"), corePath};
    ExecutedStep fileStep;
    fileStep.title = QStringLiteral("步骤 1  确认这是不是 core 文件");
    fileStep.purpose = QStringLiteral("判断选中的文件是不是程序异常退出时留下的现场记录。");
    fileStep.commandDisplay = commandFor(bins.file, QStringLiteral("file"), fileArgs);

    const QStringList readelfArgs = {QStringLiteral("-n"), QStringLiteral("-W"), QStringLiteral("--"), corePath};
    ExecutedStep readelfStep;
    readelfStep.title = QStringLiteral("步骤 2  读取记录里的停止代号");
    readelfStep.purpose = QStringLiteral("从现场记录里取出程序因何停止，以及当时的程序名称。");
    readelfStep.commandDisplay = commandFor(bins.readelf, bins.readelfName, readelfArgs);

    QStringList unstripArgs = {QStringLiteral("-n"), QStringLiteral("--core"), corePath};
    if (!exePath.isEmpty())
        unstripArgs << QStringLiteral("-e") << exePath;
    ExecutedStep unstripStep;
    unstripStep.title = QStringLiteral("步骤 3  核对程序和调试信息");
    unstripStep.purpose = QStringLiteral("查看有没有把地址翻译成步骤名称所需的对照信息。");
    unstripStep.commandDisplay = commandFor(bins.unstrip, QStringLiteral("eu-unstrip"), unstripArgs);

    QStringList gdbArgs = {QStringLiteral("-batch"), QStringLiteral("-nx"), QStringLiteral("-q"),
                           QStringLiteral("-iex"), QStringLiteral("set pagination off"),
                           QStringLiteral("-iex"), QStringLiteral("set confirm off")};
    if (symbolIsDir)
        gdbArgs << QStringLiteral("-iex") << QStringLiteral("set debug-file-directory %1").arg(gdbQuote(symbolPath));
    if (!symbolPath.isEmpty() && !symbolIsDir)
        gdbArgs << QStringLiteral("-s") << symbolPath;
    if (!exePath.isEmpty()) {
        gdbArgs << exePath << corePath;
    } else {
        gdbArgs << QStringLiteral("-c") << corePath;
    }
    gdbArgs << QStringLiteral("-ex") << QStringLiteral("thread apply all bt");
    ExecutedStep gdbStep;
    gdbStep.title = QStringLiteral("步骤 4  查看程序停在哪一个步骤");
    gdbStep.purpose = QStringLiteral("用调试器打开现场记录，看停止代号和停下前走过的步骤。");
    gdbStep.commandDisplay = commandFor(bins.gdb, QStringLiteral("gdb"), gdbArgs);

    const QFileInfo coreInfo(corePath);
    if (!coreInfo.exists() || !coreInfo.isFile()) {
        const QString reason = coreInfo.exists()
            ? QStringLiteral("请选择文件，而不是文件夹。")
            : QStringLiteral("找不到这个文件：%1").arg(corePath);
        fileStep.statusText = QStringLiteral("未执行：%1").arg(reason);
        readelfStep.statusText = QStringLiteral("未执行：文件不可用");
        unstripStep.statusText = QStringLiteral("未执行：文件不可用");
        gdbStep.statusText = QStringLiteral("未执行：文件不可用");
        fileStep.interpretation = QStringLiteral("文件不存在或不是普通文件时，不会运行分析命令。");
        report.steps = {fileStep, readelfStep, unstripStep, gdbStep};
        report.limits = limits;
        fail(reason,
             QStringLiteral("没有可读的 core 文件，所以无法判断程序为何停止。"),
             {QStringLiteral("请重新选择一个存在的 core 文件。")});
        return report;
    }

    QString headerDetail;
    const HeaderKind header = readHeader(corePath, &headerDetail);
    report.isCore = header == HeaderKind::Core;

    if (progress)
        progress(fileStep.title);
    const CommandResult fileResult = runOne(bins.file, fileArgs, 10000, &fileStep);
    if (bins.file.isEmpty())
        limits << QStringLiteral("系统里没有找到 file，文件类型改由文件头判断。");
    else if (fileResult.status == CommandStatus::FailedToStart)
        limits << QStringLiteral("file 没有跑起来：%1").arg(fileResult.errorDetail);
    if (!fileStep.output.isEmpty())
        absorbFileOutput(fileStep.output, &facts);
    fileStep.interpretation = fileInterpretation(fileStep.output, report.isCore);
    report.steps.append(fileStep);
    if (report.cancelled) {
        readelfStep.statusText = QStringLiteral("未执行：分析已取消");
        unstripStep.statusText = QStringLiteral("未执行：分析已取消");
        gdbStep.statusText = QStringLiteral("未执行：分析已取消");
        report.steps << readelfStep << unstripStep << gdbStep;
        report.limits = limits;
        fail(QStringLiteral("分析已取消。已经做完的步骤保留在下方说明里。"),
             QStringLiteral("分析被中途取消。"),
             {QStringLiteral("需要的话可以重新开始。")});
        report.cancelled = true;
        return report;
    }

    if (!report.isCore) {
        readelfStep.statusText = QStringLiteral("未执行：文件不是 core 记录");
        unstripStep.statusText = QStringLiteral("未执行：文件不是 core 记录");
        gdbStep.statusText = QStringLiteral("未执行：文件不是 core 记录");
        readelfStep.interpretation = QStringLiteral("只有 core 记录才含有程序停止时的信号和步骤。");
        report.steps << readelfStep << unstripStep << gdbStep;
        report.limits = limits;
        const QString why = headerDetail.isEmpty()
            ? QStringLiteral("选中的文件不是 Linux core 文件，不能当作崩溃现场记录来分析。")
            : QStringLiteral("选中的文件不是 Linux core 文件。%1").arg(headerDetail);
        fail(why,
             QStringLiteral("文件内容不是程序异常退出时留下的现场记录。"),
             {QStringLiteral("请重新选择 core 文件。它一般在程序意外关闭时由系统生成，名字里常带 core。"),
              QStringLiteral("如果选中的是程序本身，请把它填到“程序文件”，core 文件另选。")});
        return report;
    }

    const CommandResult readelfResult = runOne(bins.readelf, readelfArgs, 20000, &readelfStep);
    if (bins.readelf.isEmpty())
        limits << QStringLiteral("系统里没有找到 eu-readelf 或 readelf。停止代号改由 gdb 读取。");
    else if (readelfResult.status == CommandStatus::FailedToStart || readelfResult.status == CommandStatus::TimedOut)
        limits << QStringLiteral("读取停止代号的命令没有完成：%1").arg(readelfStep.statusText);
    if (!readelfStep.output.isEmpty())
        absorbReadelf(readelfStep.output, &facts);
    finishSignalNames(&facts);
    if (readelfStep.output.contains(QLatin1String("description data"))
        && !readelfStep.output.contains(QLatin1String("si_signo"))) {
        readelfStep.output = QStringLiteral("（没有翻译成可读字段，只保留开头）\n")
            + readelfStep.output.left(600);
    }
    readelfStep.interpretation = readelfInterpretation(readelfStep.output, facts);
    report.steps.append(readelfStep);
    if (report.cancelled) {
        unstripStep.statusText = QStringLiteral("未执行：分析已取消");
        gdbStep.statusText = QStringLiteral("未执行：分析已取消");
        report.steps << unstripStep << gdbStep;
        report.limits = limits;
        fail(QStringLiteral("分析已取消。已经做完的步骤保留在下方说明里。"),
             QStringLiteral("分析被中途取消。"),
             {QStringLiteral("需要的话可以重新开始。")});
        report.cancelled = true;
        return report;
    }

    const CommandResult unstripResult = runOne(bins.unstrip, unstripArgs, 20000, &unstripStep);
    if (bins.unstrip.isEmpty()) {
        limits << QStringLiteral("系统里没有找到 eu-unstrip。没有它也可以继续，只是少一项对照。");
        unstripStep.interpretation = unstripInterpretation();
    } else if (unstripResult.status == CommandStatus::Ran) {
        unstripStep.interpretation = unstripInterpretation();
        if (unstripStep.output.contains(QLatin1String(" - ")) || unstripStep.output.contains(QLatin1String(" . "))) {
            unstripStep.interpretation += QStringLiteral(" 输出里的 “-” 或 “.” 表示对应的程序或对照信息缺失。");
        }
    } else {
        unstripStep.interpretation = QStringLiteral("这条对照命令没有完成，不影响后面用调试器查看停止原因。");
        limits << QStringLiteral("eu-unstrip 没有完成：%1").arg(unstripStep.statusText);
    }
    report.steps.append(unstripStep);
    if (report.cancelled) {
        gdbStep.statusText = QStringLiteral("未执行：分析已取消");
        report.steps << gdbStep;
        report.limits = limits;
        fail(QStringLiteral("分析已取消。已经做完的步骤保留在下方说明里。"),
             QStringLiteral("分析被中途取消。"),
             {QStringLiteral("需要的话可以重新开始。")});
        report.cancelled = true;
        return report;
    }

    const bool gdbMissing = bins.gdb.isEmpty();
    const CommandResult gdbResult = runOne(bins.gdb, gdbArgs, 60000, &gdbStep);
    if (gdbMissing)
        limits << QStringLiteral("系统里没有找到 gdb，所以看不到程序停在哪一个步骤。");
    else if (gdbResult.status == CommandStatus::FailedToStart || gdbResult.status == CommandStatus::TimedOut)
        limits << QStringLiteral("调试器没有完成：%1").arg(gdbStep.statusText);
    if (!gdbStep.output.isEmpty())
        absorbGdb(gdbStep.output, &facts);
    finishSignalNames(&facts);
    gdbStep.interpretation = gdbMissing
        ? QStringLiteral("安装 gdb 后重新分析，才能看到停下的步骤。")
        : gdbInterpretation(facts);
    report.steps.append(gdbStep);

    if (report.cancelled) {
        report.limits = limits;
        fail(QStringLiteral("分析已取消。已经做完的步骤保留在下方说明里。"),
             QStringLiteral("分析被中途取消。"),
             {QStringLiteral("需要的话可以重新开始。")});
        report.cancelled = true;
        return report;
    }

    finishSignalNames(&facts);
    report.signalNumber = facts.signalNumber;
    report.signalName = facts.signalName;
    report.whereInfo = whereText(facts);
    report.errorInfo = errorText(facts);
    report.cause = causeText(facts);
    report.suggestionText = numberSuggestions(suggestionList(facts, gdbMissing));
    report.limits = limits;
    report.ok = facts.signalNumber > 0 || !facts.signalName.isEmpty() || !facts.frames.isEmpty();
    if (!report.ok) {
        report.errorInfo = QStringLiteral("这是一份 core 文件，但这次没能读出程序为何停止。");
        report.cause = QStringLiteral("现场记录已确认，可是读取停止代号的命令没有给出可用结果。");
        if (report.suggestionText.isEmpty()) {
            report.suggestionText = numberSuggestions(
                {QStringLiteral("请确认已安装 gdb 或 eu-readelf，然后重新分析。"),
                 QStringLiteral("请先不要删除这份 core 文件。")});
        }
    }
    report.rendered = renderReport(report);
    return report;
}

} // namespace tw
