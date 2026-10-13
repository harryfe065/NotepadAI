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

#include "AcpAgentIcons.h"

#include <QColor>
#include <QFileInfo>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

#include "AcpAgentBrand.h"
#include "AcpAgentDefinition.h"


namespace {

// Renders the SVG once per size and composites `color` over it with SourceIn.
// Qt's icon engine resolves currentColor to opaque black and never follows
// theme switches, so monochrome marks are re-tinted per palette instead.
QIcon tintedIcon(const QString &svgPath, const QColor &color)
{
    const QIcon source(svgPath);
    QIcon dst;
    for (const int sz : {16, 20, 24, 32, 40, 48, 64}) {
        QPixmap pm = source.pixmap(sz, sz);
        if (pm.isNull()) {
            continue;
        }
        QPainter p(&pm);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(pm.rect(), color);
        p.end();
        dst.addPixmap(pm);
    }
    return dst;
}

// Process-wide cache keyed by path (+ tint). A handful of brands times the
// themes the user switches through, plus custom images, so it stays tiny.
QHash<QString, QIcon> &iconCache()
{
    static QHash<QString, QIcon> cache;
    return cache;
}

QIcon cachedIcon(const QString &path, const QColor &tint = QColor())
{
    QString key = path;
    if (tint.isValid()) {
        key += QLatin1Char('|');
        key += QString::number(tint.rgba(), 16);
    }
    QHash<QString, QIcon> &cache = iconCache();
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) {
        return it.value();
    }
    const QIcon icon = tint.isValid() ? tintedIcon(path, tint) : QIcon(path);
    cache.insert(key, icon);
    return icon;
}

// Null when the file is missing so the caller can fall back to the brand.
QIcon customFileIcon(const QString &path)
{
    QHash<QString, QIcon> &cache = iconCache();
    const QString key = QStringLiteral("file|") + path;
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) {
        return it.value();
    }
    const QIcon icon = QFileInfo(path).isFile() ? QIcon(path) : QIcon();
    cache.insert(key, icon);
    return icon;
}

bool isDarkPalette(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

} // namespace

QIcon acpAgentBrandIcon(const QString &brandKey, const QPalette &palette)
{
    const QColor fg = palette.color(QPalette::ButtonText);
    if (brandKey == QLatin1String("claude-code")) {
        return cachedIcon(QStringLiteral(":/icons/claude.svg"));
    }
    if (brandKey == QLatin1String("codex")) {
        return cachedIcon(QStringLiteral(":/icons/agents/codex.svg"), fg);
    }
    if (brandKey == QLatin1String("opencode")) {
        return cachedIcon(isDarkPalette(palette)
            ? QStringLiteral(":/icons/agents/opencode-dark.svg")
            : QStringLiteral(":/icons/agents/opencode-light.svg"));
    }
    if (brandKey == QLatin1String("oh-my-pi")) {
        return cachedIcon(QStringLiteral(":/icons/agents/oh-my-pi.svg"));
    }
    if (brandKey == QLatin1String("pi")) {
        return cachedIcon(QStringLiteral(":/icons/agents/pi.svg"), fg);
    }
    return cachedIcon(QStringLiteral(":/icons/agents/agent.svg"), fg);
}

QIcon acpAgentIcon(const AcpAgentDefinition &def, const QPalette &palette)
{
    const QString icon = def.icon.trimmed();
    if (!icon.isEmpty() && !isAcpAgentBrandKey(icon)) {
        QIcon custom = customFileIcon(icon);
        if (!custom.isNull()) {
            return custom;
        }
        // Missing file: still show the agent's own mark rather than a blank.
        return acpAgentBrandIcon(detectAcpAgentBrand(def), palette);
    }
    return acpAgentBrandIcon(acpAgentBrandKey(def), palette);
}
