#pragma once

#include "core/status_board.h"

#include <QColor>
#include <QMap>
#include <QString>

namespace tw {

// 工业工作台主题：深石墨外框（顶栏、工具栏、状态栏）围住浅色工作区，
// 琥珀色只用于“当前位置”和焦点。颜色集中在这里，QSS 用 @名称@ 引用，
// 绘制代码（工具栏、图标）直接读同一份数值，避免两处各写一套颜色。
struct Theme
{
    // 外框
    QColor chrome{0x20, 0x26, 0x2d};
    QColor chromeRaised{0x2a, 0x31, 0x3a};
    QColor chromeHover{0x34, 0x3c, 0x47};
    QColor chromeSelected{0x3c, 0x47, 0x55};
    QColor chromeBorder{0x12, 0x16, 0x1b};
    QColor chromeText{0xe8, 0xeb, 0xef};
    QColor chromeMuted{0x97, 0xa2, 0xaf};
    QColor chromeDisabled{0x6b, 0x75, 0x80};
    QColor accent{0xe0, 0xa0, 0x28};

    // 工作区
    QColor surface{0xee, 0xf0, 0xf3};
    QColor panel{0xff, 0xff, 0xff};
    QColor panelAlt{0xf6, 0xf7, 0xf9};
    QColor border{0xcd, 0xd2, 0xd9};
    QColor text{0x1f, 0x29, 0x33};
    QColor textMuted{0x5d, 0x69, 0x75};
    QColor disabledText{0x9a, 0xa4, 0xaf};
    QColor primary{0x2e, 0x6d, 0xa4};
    QColor primaryHover{0x26, 0x59, 0x8a};
    QColor danger{0xb3, 0x26, 0x1e};

    // 深色状态栏上的状态色
    QColor statusIdle{0xaa, 0xb4, 0xc0};
    QColor statusInfo{0x86, 0xb8, 0xea};
    QColor statusBusy{0xf2, 0xb8, 0x4b};
    QColor statusSuccess{0x78, 0xd4, 0x92};
    QColor statusError{0xff, 0x8f, 0x85};

    QString fontFamily = QStringLiteral(
        "\"Noto Sans CJK SC\", \"Source Han Sans SC\", \"WenQuanYi Micro Hei\", "
        "\"Microsoft YaHei\", sans-serif");
    QString monoFamily = QStringLiteral(
        "\"Noto Sans Mono CJK SC\", \"DejaVu Sans Mono\", monospace");
    int fontPixelSize = 13;

    static const Theme &workbench();

    QColor statusColor(StatusState state) const;

    // 名称 -> 值，用于替换 QSS 里的 @名称@。
    QMap<QString, QString> tokens() const;

    // 读取 :/tengwangge/theme.qss 并替换占位符；资源缺失时返回空串。
    QString styleSheet() const;

    // 用 tokens() 替换模板里的 @名称@，返回替换后的文本。
    static QString expand(const QString &templateText, const QMap<QString, QString> &tokens);
};

} // namespace tw
