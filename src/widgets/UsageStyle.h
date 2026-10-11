#ifndef USAGE_STYLE_H
#define USAGE_STYLE_H

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSize>

#include "ai/ClaudeUsage.h"

// Shared look of the Claude usage button + popover.
namespace usagestyle {

// Soft status accents (ui-dna: the one allowed non-palette colors). Readable on
// both light and dark chrome.
inline QColor warningColor() { return QColor(0xd2, 0x99, 0x22); }
inline QColor dangerColor() { return QColor(0xe5, 0x53, 0x4b); }

// SVGs that declare `currentColor` render opaque black in Qt's icon engine, so
// re-render and tint with `color` (SourceIn keeps the stroke alpha).
inline QPixmap tintedPixmap(const QString &svgPath, const QColor &color, int logicalSize, qreal dpr)
{
    QPixmap pm = QIcon(svgPath).pixmap(QSize(logicalSize, logicalSize), dpr);
    if (pm.isNull())
        return pm;
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), color);
    return pm;
}

// Fixed-color (brand) glyph, no tint.
inline QPixmap brandPixmap(const QString &svgPath, int logicalSize, qreal dpr)
{
    return QIcon(svgPath).pixmap(QSize(logicalSize, logicalSize), dpr);
}

} // namespace usagestyle

#endif // USAGE_STYLE_H
