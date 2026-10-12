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

#ifndef TERMINALCONTRAST_H
#define TERMINALCONTRAST_H

#include <QHash>
#include <QRgb>

// Minimum text contrast, as in Zed's terminal: ANSI/default text whose APCA
// lightness contrast against its background is below a floor is nudged toward
// black or white until it reads (dark blue on black, yellow on white).
namespace TerminalContrast {

// APCA-W3 (0.0.98G) lightness contrast Lc of `text` on `bg`, roughly -108..106.
// Positive for dark text on a light background.
double apca(QRgb text, QRgb bg);

// `fg` moved toward black or white just enough for |Lc| >= minLc; `fg`
// unchanged when it already passes or minLc <= 0.
QRgb ensureMinimum(QRgb fg, QRgb bg, double minLc);

} // namespace TerminalContrast

// Memoises ensureMinimum per (fg, bg) pair: the paint path sees a handful of
// distinct pairs, so after the first frame every lookup is one hash probe.
class TerminalContrastCache
{
public:
    void setMinimum(double minLc);
    double minimum() const { return m_min; }
    bool enabled() const { return m_min > 0; }
    QRgb adjust(QRgb fg, QRgb bg);

private:
    double m_min = 0;
    QHash<quint64, QRgb> m_cache;
};

#endif
