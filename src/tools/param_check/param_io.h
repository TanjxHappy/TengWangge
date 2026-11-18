#pragma once

#include <QByteArray>
#include <QString>

namespace tw {

struct LoadedParamFile {
    QString text;
    bool utf8Bom = false;
    bool crlf = false;
    // 原始字节不是合法 UTF-8。text 只是给用户看的替换预览，不能直接写回。
    bool lossyUtf8 = false;
};

// 读入参数文件。失败时不改动 out，并写入 error。
bool readParamFile(const QString &path, LoadedParamFile *loaded, QString *error);

// 把文本编码成要写回的字节：UTF-8，可选 BOM，可选 CRLF。
QByteArray encodeParamText(const QString &text, bool utf8Bom, bool crlf);

// 同一目录内先写临时文件并 fsync，再 rename 覆盖目标。失败时删除临时文件。
bool writeParamFileAtomically(const QString &path, const QByteArray &bytes, QString *error);

// 工具自己的界面状态（上次目录、分割条），只放在 dataDir() 里。
bool loadParamSession(const QString &dataDir, QString *lastDirectory, QByteArray *splitter);
void saveParamSession(const QString &dataDir, const QString &lastDirectory, const QByteArray &splitter);

} // namespace tw
