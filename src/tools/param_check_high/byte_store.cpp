#include "tools/param_check_high/byte_store.h"

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

QString statePath(const QString &dataDir)
{
    return QDir(dataDir).filePath(QStringLiteral("workspace.json"));
}

} // namespace

bool readParameterFile(const QString &path, LoadedText *loaded, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (loaded == nullptr)
        return fail(QStringLiteral("内部错误：没有地方存放读取结果。"));

    const QFileInfo info(path);
    if (!info.exists())
        return fail(QStringLiteral("没有找到这个文件：%1").arg(path));
    if (!info.isFile())
        return fail(QStringLiteral("这不是一个普通文件：%1").arg(path));

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("没有权限读取这个文件：%1").arg(file.errorString()));
    const QByteArray raw = file.readAll();
    if (raw.contains('\0'))
        return fail(QStringLiteral("文件里有空字符，看起来不是文本参数。没有修改原文件。"));

    QByteArray body = raw;
    bool bom = false;
    if (body.startsWith("\xEF\xBB\xBF")) {
        bom = true;
        body.remove(0, 3);
    }
    const bool crlf = body.contains("\r\n") || body.contains('\r');

    QTextCodec *codec = QTextCodec::codecForName("UTF-8");
    if (codec == nullptr)
        return fail(QStringLiteral("当前环境没有 UTF-8 解码器。"));
    QTextCodec::ConverterState state;
    QString text = codec->toUnicode(body.constData(), body.size(), &state);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    loaded->text = text;
    loaded->bom = bom;
    loaded->crlf = crlf;
    loaded->notUtf8 = state.invalidChars > 0;
    return true;
}

QByteArray encodeParameterText(const QString &text, bool bom, bool crlf)
{
    QString normalized = text;
    normalized.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (crlf)
        normalized.replace(QLatin1Char('\n'), QLatin1String("\r\n"));

    QByteArray bytes;
    if (bom)
        bytes.append("\xEF\xBB\xBF", 3);
    bytes.append(normalized.toUtf8());
    return bytes;
}

bool commitParameterFile(const QString &path, const QByteArray &bytes, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    const QFileInfo info(path);
    const QString directory = info.absolutePath();
    const QString fileName = info.fileName();
    if (fileName.isEmpty() || directory.isEmpty() || !QDir(directory).exists())
        return fail(QStringLiteral("保存目录不存在：%1。原文件没有改动。").arg(directory));

    const QString temporary = QDir(directory).filePath(
        QStringLiteral(".%1.high-%2").arg(fileName, QUuid::createUuid().toString(QUuid::Id128)));
    QFile out(temporary);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("无法准备临时文件，原文件没有改动。（%1）").arg(out.errorString()));

    const QFile::Permissions permissions = info.permissions();
    const bool keepPermissions = info.exists();
    if (keepPermissions)
        out.setPermissions(permissions);

    if (out.write(bytes) != bytes.size() || !out.flush()) {
        out.close();
        QFile::remove(temporary);
        return fail(QStringLiteral("写入中断，原文件没有改动。"));
    }
    if (::fsync(out.handle()) != 0) {
        out.close();
        QFile::remove(temporary);
        return fail(QStringLiteral("同步到磁盘失败，原文件没有改动。"));
    }
    out.close();
    if (keepPermissions)
        QFile::setPermissions(temporary, permissions);

    const QByteArray from = QFile::encodeName(temporary);
    const QByteArray to = QFile::encodeName(info.absoluteFilePath());
    if (::rename(from.constData(), to.constData()) != 0) {
        const QString reason = QString::fromLocal8Bit(std::strerror(errno));
        QFile::remove(temporary);
        return fail(QStringLiteral("无法替换原文件，原文件没有改动。（%1）").arg(reason));
    }

    const int directoryFd = ::open(QFile::encodeName(directory).constData(), O_RDONLY | O_DIRECTORY);
    if (directoryFd >= 0) {
        ::fsync(directoryFd);
        ::close(directoryFd);
    }
    return true;
}

bool loadEditorState(const QString &dataDir, QString *lastDirectory, QByteArray *splitter)
{
    if (lastDirectory)
        lastDirectory->clear();
    if (splitter)
        splitter->clear();
    if (dataDir.isEmpty())
        return false;

    QFile file(statePath(dataDir));
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return false;
    const QJsonObject object = document.object();
    if (lastDirectory)
        *lastDirectory = object.value(QStringLiteral("lastDirectory")).toString();
    if (splitter) {
        *splitter = QByteArray::fromBase64(object.value(QStringLiteral("splitter")).toString().toLatin1());
    }
    return true;
}

void saveEditorState(const QString &dataDir, const QString &lastDirectory, const QByteArray &splitter)
{
    if (dataDir.isEmpty())
        return;
    QJsonObject object;
    object.insert(QStringLiteral("lastDirectory"), lastDirectory);
    object.insert(QStringLiteral("splitter"), QString::fromLatin1(splitter.toBase64()));
    QString error;
    commitParameterFile(statePath(dataDir), QJsonDocument(object).toJson(QJsonDocument::Indented), &error);
}

} // namespace tw
