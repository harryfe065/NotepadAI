/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "TerminalIcons.h"

#include <QHash>
#include <QPainter>
#include <QPixmap>

namespace {
constexpr qsizetype kMaxCachedIcons = 128;
} // namespace

QIcon terminalTintedIcon(const QString &svgPath, const QColor &color, const QColor &dotColor)
{
    static QHash<QString, QIcon> cache;
    const QString key = svgPath + QLatin1Char('|') + QString::number(color.rgba(), 16) + QLatin1Char('|')
        + (dotColor.isValid() ? QString::number(dotColor.rgba(), 16) : QString());
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) {
        return it.value();
    }

    const QIcon source(svgPath);
    QIcon out;
    if (!source.isNull()) {
        for (int size : {16, 20, 24, 32, 48, 64}) {
            QPixmap pm = source.pixmap(size, size);
            if (pm.isNull()) continue;
            QPainter p(&pm);
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(pm.rect(), color);
            if (dotColor.isValid()) {
                p.setCompositionMode(QPainter::CompositionMode_SourceOver);
                p.setRenderHint(QPainter::Antialiasing, true);
                const qreal d = size * 0.42;
                p.setPen(Qt::NoPen);
                p.setBrush(dotColor);
                p.drawEllipse(QRectF(size - d, 0, d, d));
            }
            p.end();
            out.addPixmap(pm);
        }
    }
    if (cache.size() >= kMaxCachedIcons) {
        cache.clear();
    }
    cache.insert(key, out);
    return out;
}
