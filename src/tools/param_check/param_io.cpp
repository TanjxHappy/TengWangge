#include "tools/param_check/param_io.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextCodec>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace tw {
namespace {

QString sessionPath(const QString &dataDir)
{
    return QDir(dataDir).filePath(QStringLiteral("session.json"));
}

} // namespace

bool readParamFile(const QString &path, LoadedParamFile *loaded, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!loaded)
        return fail(QStringLiteral("内部错误：缺少读取结果"));

    const QFileInfo info(path);
    if (!info.exists())
        return fail(QStringLiteral("文件不存在：") + path);
    if (!info.isFile())
        return fail(QStringLiteral("不是普通文件：") + path);

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法读取文件：") + file.errorString());
    const QByteArray raw = file.readAll();
    if (raw.contains('\0'))
        return fail(QStringLiteral("文件包含空字节，不能当作文本参数打开。"));

    QByteArray body = raw;
    bool bom = false;
    if (body.startsWith("\xEF\xBB\xBF")) {
        bom = true;
        body.remove(0, 3);
    }
    const bool crlf = body.contains("\r\n") || body.contains('\r');

    QTextCodec *codec = QTextCodec::codecForName("UTF-8");
    if (!codec)
        return fail(QStringLiteral("当前环境没有 UTF-8 解码器。"));
    QTextCodec::ConverterState state;
    QString text = codec->toUnicode(body.constData(), body.size(), &state);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    loaded->text = text;
    loaded->utf8Bom = bom;
    loaded->crlf = crlf;
    loaded->lossyUtf8 = state.invalidChars > 0;
    return true;
}

QByteArray encodeParamText(const QString &text, bool utf8Bom, bool crlf)
{
    QString normalized = text;
    normalized.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (crlf)
        normalized.replace(QLatin1Char('\n'), QLatin1String("\r\n"));

    QByteArray bytes;
    if (utf8Bom)
        bytes.append("\xEF\xBB\xBF", 3);
    bytes.append(normalized.toUtf8());
    return bytes;
}

bool writeParamFileAtomically(const QString &path, const QByteArray &bytes, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    const QFileInfo info(path);
    const QString directory = info.absolutePath();
    if (info.fileName().isEmpty() || directory.isEmpty() || !QDir(directory).exists())
        return fail(QStringLiteral("保存目录不存在：") + directory);

    const QString temporary = QDir(directory).filePath(
        QStringLiteral(".%1.twtmp-%2").arg(info.fileName(), QUuid::createUuid().toString(QUuid::Id128)));
    QFile out(temporary);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("无法写入临时文件：") + out.errorString());
    if (info.exists())
        out.setPermissions(info.permissions());

    const qint64 written = out.write(bytes);
    if (written != bytes.size() || !out.flush()) {
        out.close();
        QFile::remove(temporary);
        return fail(QStringLiteral("写入临时文件不完整，原文件未改动。"));
    }
    if (::fsync(out.handle()) != 0) {
        out.close();
        QFile::remove(temporary);
        return fail(QStringLiteral("同步临时文件失败，原文件未改动。"));
    }
    out.close();

    // POSIX rename 在同一目录里替换已有文件，且是原子的。QFile::rename 在目标已存在时会直接失败。
    if (::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(path).constData()) != 0) {
        const QString reason = QString::fromLocal8Bit(std::strerror(errno));
        QFile::remove(temporary);
        return fail(QStringLiteral("无法替换原文件：") + path + QStringLiteral("（") + reason + QStringLiteral("）"));
    }

    const int directoryFd = ::open(QFile::encodeName(directory).constData(), O_RDONLY | O_DIRECTORY);
    if (directoryFd >= 0) {
        ::fsync(directoryFd);
        ::close(directoryFd);
    }
    return true;
}

bool loadParamSession(const QString &dataDir, QString *lastDirectory, QByteArray *splitter)
{
    if (lastDirectory)
        lastDirectory->clear();
    if (splitter)
        splitter->clear();
    if (dataDir.isEmpty())
        return false;

    QFile file(sessionPath(dataDir));
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return false;
    const QJsonObject object = document.object();
    if (lastDirectory)
        *lastDirectory = object.value(QStringLiteral("lastDirectory")).toString();
    if (splitter) {
        *splitter = QByteArray::fromBase64(
            object.value(QStringLiteral("splitter")).toString().toLatin1());
    }
    return true;
}

void saveParamSession(const QString &dataDir, const QString &lastDirectory, const QByteArray &splitter)
{
    if (dataDir.isEmpty())
        return;
    QJsonObject object;
    object.insert(QStringLiteral("lastDirectory"), lastDirectory);
    object.insert(QStringLiteral("splitter"), QString::fromLatin1(splitter.toBase64()));
    QString error;
    writeParamFileAtomically(sessionPath(dataDir), QJsonDocument(object).toJson(QJsonDocument::Indented), &error);
}

} // namespace tw
