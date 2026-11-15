#pragma once

#include <QByteArray>
#include <QString>

namespace tw {

struct LoadedText {
    QString text;
    bool bom = false;
    bool crlf = false;
    // 原始字节不是合法 UTF-8。text 只是替换后的预览，不能写回原路径。
    bool notUtf8 = false;
};

// 读参数文件。失败时不改动 loaded，并写入 error。
bool readParameterFile(const QString &path, LoadedText *loaded, QString *error);

// 把编辑器里的文本编成要写回的字节：UTF-8，可选 BOM，可选 CRLF。
QByteArray encodeParameterText(const QString &text, bool bom, bool crlf);

// 同一目录里先写临时文件并 fsync，再 rename 替换目标。失败时删掉临时文件。
bool commitParameterFile(const QString &path, const QByteArray &bytes, QString *error);

// 界面状态（上次目录、分割条）。只放在 dataDir() 里，不存文件内容。
bool loadEditorState(const QString &dataDir, QString *lastDirectory, QByteArray *splitter);
void saveEditorState(const QString &dataDir, const QString &lastDirectory, const QByteArray &splitter);

} // namespace tw
