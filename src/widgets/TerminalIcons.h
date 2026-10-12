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

#ifndef TERMINALICONS_H
#define TERMINALICONS_H

#include <QColor>
#include <QIcon>
#include <QString>

// SVG with stroke="currentColor" rendered in `color` (SourceIn tint, per the
// CLAUDE.md icon rule); a valid `dotColor` adds a badge dot top-right.
// Results are memoised per (svg, colour, dot), so re-tinting on every tab
// update or theme switch costs a hash lookup after the first time.
QIcon terminalTintedIcon(const QString &svgPath, const QColor &color, const QColor &dotColor = QColor());

#endif
