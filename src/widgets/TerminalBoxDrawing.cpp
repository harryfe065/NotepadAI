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

#include "TerminalBoxDrawing.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace TerminalBoxDrawing {

namespace {

constexpr uint8_t A(int l, int r, int u, int d)
{
    return static_cast<uint8_t>(l | (r << 2) | (u << 4) | (d << 6));
}

// Arms for U+2500..U+257F; 0 marks dashes, arcs and diagonals.
constexpr uint8_t kArms[128] = {
    // 2500
    A(1,1,0,0), A(2,2,0,0), A(0,0,1,1), A(0,0,2,2), 0, 0, 0, 0,
    0, 0, 0, 0, A(0,1,0,1), A(0,2,0,1), A(0,1,0,2), A(0,2,0,2),
    // 2510
    A(1,0,0,1), A(2,0,0,1), A(1,0,0,2), A(2,0,0,2), A(0,1,1,0), A(0,2,1,0), A(0,1,2,0), A(0,2,2,0),
    A(1,0,1,0), A(2,0,1,0), A(1,0,2,0), A(2,0,2,0), A(0,1,1,1), A(0,2,1,1), A(0,1,2,1), A(0,1,1,2),
    // 2520
    A(0,1,2,2), A(0,2,2,1), A(0,2,1,2), A(0,2,2,2), A(1,0,1,1), A(2,0,1,1), A(1,0,2,1), A(1,0,1,2),
    A(1,0,2,2), A(2,0,2,1), A(2,0,1,2), A(2,0,2,2), A(1,1,0,1), A(2,1,0,1), A(1,2,0,1), A(2,2,0,1),
    // 2530
    A(1,1,0,2), A(2,1,0,2), A(1,2,0,2), A(2,2,0,2), A(1,1,1,0), A(2,1,1,0), A(1,2,1,0), A(2,2,1,0),
    A(1,1,2,0), A(2,1,2,0), A(1,2,2,0), A(2,2,2,0), A(1,1,1,1), A(2,1,1,1), A(1,2,1,1), A(2,2,1,1),
    // 2540
    A(1,1,2,1), A(1,1,1,2), A(1,1,2,2), A(2,1,2,1), A(1,2,2,1), A(2,1,1,2), A(1,2,1,2), A(2,2,2,1),
    A(2,2,1,2), A(2,1,2,2), A(1,2,2,2), A(2,2,2,2), 0, 0, 0, 0,
    // 2550
    A(3,3,0,0), A(0,0,3,3), A(0,3,0,1), A(0,1,0,3), A(0,3,0,3), A(3,0,0,1), A(1,0,0,3), A(3,0,0,3),
    A(0,3,1,0), A(0,1,3,0), A(0,3,3,0), A(3,0,1,0), A(1,0,3,0), A(3,0,3,0), A(0,3,1,1), A(0,1,3,3),
    // 2560
    A(0,3,3,3), A(3,0,1,1), A(1,0,3,3), A(3,0,3,3), A(3,3,0,1), A(1,1,0,3), A(3,3,0,3), A(3,3,1,0),
    A(1,1,3,0), A(3,3,3,0), A(3,3,1,1), A(1,1,3,3), A(3,3,3,3), 0, 0, 0,
    // 2570
    0, 0, 0, 0, A(1,0,0,0), A(0,0,1,0), A(0,1,0,0), A(0,0,0,1),
    A(2,0,0,0), A(0,0,2,0), A(0,2,0,0), A(0,0,0,2), A(1,2,0,0), A(0,0,1,2), A(2,1,0,0), A(0,0,2,1),
};

struct Grid {
    qreal x, y, w, h;
    qreal t1, t2;   // light and heavy stroke
    qreal dpr;

    qreal snap(qreal v) const { return std::round(v * dpr) / dpr; }

    // Leading edge of a centred band of thickness `t`.
    qreal hBand(qreal t) const { return snap(y + (h - t) / 2); }
    qreal vBand(qreal t) const { return snap(x + (w - t) / 2); }
    qreal midX() const { return snap(x + w / 2); }
    qreal midY() const { return snap(y + h / 2); }
};

void fillRect(QPainter &p, qreal l, qreal t, qreal r, qreal b, const QColor &c)
{
    if (r > l && b > t) {
        p.fillRect(QRectF(l, t, r - l, b - t), c);
    }
}

qreal strokeFor(const Grid &g, int weight)
{
    return weight == 2 ? g.t2 : g.t1;
}

void paintArms(QPainter &p, const Grid &g, const Arms &a, const QColor &c)
{
    const qreal right = g.x + g.w;
    const qreal bottom = g.y + g.h;
    const int vMax = std::max(a.up, a.down);
    const int hMax = std::max(a.left, a.right);
    const bool vDouble = a.up == 3 || a.down == 3;
    const bool hDouble = a.left == 3 || a.right == 3;

    // Double lines: two light strokes with a light gap, centred.
    const qreal vD = g.vBand(3 * g.t1);
    const qreal hD = g.hBand(3 * g.t1);

    // Where a horizontal arm meets the vertical structure.
    auto hInnerEnd = [&](bool leftSide) -> qreal {
        if (vMax == 0) return g.midX();
        if (vDouble) return leftSide ? vD + g.t1 : vD + 2 * g.t1;
        const qreal t = strokeFor(g, vMax);
        const qreal l = g.vBand(t);
        return leftSide ? l + t : l;
    };
    auto vInnerEnd = [&](bool topSide) -> qreal {
        if (hMax == 0) return g.midY();
        if (hDouble) return topSide ? hD + g.t1 : hD + 2 * g.t1;
        const qreal t = strokeFor(g, hMax);
        const qreal top = g.hBand(t);
        return topSide ? top + t : top;
    };

    // Single and heavy arms.
    const bool hThrough = a.left && a.right && a.left != 3 && a.right != 3 && vDouble;
    const bool vThrough = a.up && a.down && a.up != 3 && a.down != 3 && hDouble;
    if (a.left && a.left != 3) {
        const qreal t = strokeFor(g, a.left);
        const qreal top = g.hBand(t);
        fillRect(p, g.x, top, hThrough ? g.midX() : hInnerEnd(true), top + t, c);
    }
    if (a.right && a.right != 3) {
        const qreal t = strokeFor(g, a.right);
        const qreal top = g.hBand(t);
        fillRect(p, hThrough ? g.midX() : hInnerEnd(false), top, right, top + t, c);
    }
    if (a.up && a.up != 3) {
        const qreal t = strokeFor(g, a.up);
        const qreal l = g.vBand(t);
        fillRect(p, l, g.y, l + t, vThrough ? g.midY() : vInnerEnd(true), c);
    }
    if (a.down && a.down != 3) {
        const qreal t = strokeFor(g, a.down);
        const qreal l = g.vBand(t);
        fillRect(p, l, vThrough ? g.midY() : vInnerEnd(false), l + t, bottom, c);
    }

    // Double arms. The stroke on the inside of a corner stops at the near
    // perpendicular stroke, the outside one runs to the far stroke.
    const qreal upperTop = hD;
    const qreal lowerTop = hD + 2 * g.t1;
    const qreal leftL = vD;
    const qreal rightL = vD + 2 * g.t1;
    const bool upD = a.up == 3;
    const bool downD = a.down == 3;
    const bool leftD = a.left == 3;
    const bool rightD = a.right == 3;
    auto hSingleCross = [&](bool leftSide) -> qreal {
        // A single/heavy vertical (or none) crossing a double horizontal.
        if (vMax == 0) return g.midX();
        const qreal t = strokeFor(g, vMax);
        const qreal l = g.vBand(t);
        return leftSide ? l + t : l;
    };
    auto vSingleCross = [&](bool topSide) -> qreal {
        if (hMax == 0) return g.midY();
        const qreal t = strokeFor(g, hMax);
        const qreal top = g.hBand(t);
        return topSide ? top + t : top;
    };

    if (leftD) {
        qreal upperEnd, lowerEnd;
        if (vDouble) {
            upperEnd = upD ? leftL + g.t1 : rightL + g.t1;
            lowerEnd = downD ? leftL + g.t1 : rightL + g.t1;
        } else {
            upperEnd = lowerEnd = rightD ? g.midX() : hSingleCross(true);
        }
        fillRect(p, g.x, upperTop, upperEnd, upperTop + g.t1, c);
        fillRect(p, g.x, lowerTop, lowerEnd, lowerTop + g.t1, c);
    }
    if (rightD) {
        qreal upperStart, lowerStart;
        if (vDouble) {
            upperStart = upD ? rightL : leftL;
            lowerStart = downD ? rightL : leftL;
        } else {
            upperStart = lowerStart = leftD ? g.midX() : hSingleCross(false);
        }
        fillRect(p, upperStart, upperTop, right, upperTop + g.t1, c);
        fillRect(p, lowerStart, lowerTop, right, lowerTop + g.t1, c);
    }
    if (upD) {
        qreal leftEnd, rightEnd;
        if (hDouble) {
            leftEnd = leftD ? upperTop + g.t1 : lowerTop + g.t1;
            rightEnd = rightD ? upperTop + g.t1 : lowerTop + g.t1;
        } else {
            leftEnd = rightEnd = downD ? g.midY() : vSingleCross(true);
        }
        fillRect(p, leftL, g.y, leftL + g.t1, leftEnd, c);
        fillRect(p, rightL, g.y, rightL + g.t1, rightEnd, c);
    }
    if (downD) {
        qreal leftStart, rightStart;
        if (hDouble) {
            leftStart = leftD ? lowerTop : upperTop;
            rightStart = rightD ? lowerTop : upperTop;
        } else {
            leftStart = rightStart = upD ? g.midY() : vSingleCross(false);
        }
        fillRect(p, leftL, leftStart, leftL + g.t1, bottom, c);
        fillRect(p, rightL, rightStart, rightL + g.t1, bottom, c);
    }
}

void paintDashes(QPainter &p, const Grid &g, bool horizontal, int segments, int weight, const QColor &c)
{
    const qreal t = strokeFor(g, weight);
    const qreal span = horizontal ? g.w : g.h;
    const qreal seg = span / segments;
    const qreal gap = std::max(1.0 / g.dpr, g.snap(seg * 0.3));
    for (int i = 0; i < segments; ++i) {
        const qreal a = g.snap((horizontal ? g.x : g.y) + i * seg + gap / 2);
        const qreal b = g.snap((horizontal ? g.x : g.y) + (i + 1) * seg - gap / 2);
        if (horizontal) {
            const qreal top = g.hBand(t);
            fillRect(p, a, top, b, top + t, c);
        } else {
            const qreal l = g.vBand(t);
            fillRect(p, l, a, l + t, b, c);
        }
    }
}

void paintArc(QPainter &p, const Grid &g, char32_t cp, const QColor &c)
{
    // Centre of the stroke, so the arc meets straight neighbours exactly.
    const qreal cx = g.vBand(g.t1) + g.t1 / 2;
    const qreal cy = g.hBand(g.t1) + g.t1 / 2;
    const qreal r = std::min(g.w, g.h) / 2;
    const qreal right = g.x + g.w;
    const qreal bottom = g.y + g.h;

    QPainterPath path;
    switch (cp) {
    case 0x256D: // ╭ right and down
        path.moveTo(right, cy);
        path.lineTo(cx + r, cy);
        path.arcTo(QRectF(cx, cy, 2 * r, 2 * r), 90, 90);
        path.lineTo(cx, bottom);
        break;
    case 0x256E: // ╮ left and down
        path.moveTo(g.x, cy);
        path.lineTo(cx - r, cy);
        path.arcTo(QRectF(cx - 2 * r, cy, 2 * r, 2 * r), 90, -90);
        path.lineTo(cx, bottom);
        break;
    case 0x256F: // ╯ left and up
        path.moveTo(g.x, cy);
        path.lineTo(cx - r, cy);
        path.arcTo(QRectF(cx - 2 * r, cy - 2 * r, 2 * r, 2 * r), 270, 90);
        path.lineTo(cx, g.y);
        break;
    default:     // ╰ right and up
        path.moveTo(right, cy);
        path.lineTo(cx + r, cy);
        path.arcTo(QRectF(cx, cy - 2 * r, 2 * r, 2 * r), 270, -90);
        path.lineTo(cx, g.y);
        break;
    }
    QPen pen(c, g.t1, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.strokePath(path, pen);
}

void paintDiagonal(QPainter &p, const Grid &g, char32_t cp, const QColor &c)
{
    QPen pen(c, g.t1, Qt::SolidLine, Qt::FlatCap);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(pen);
    // Overshoot the corners so diagonals tile across cells.
    const qreal ox = g.w / g.h * (g.t1 / 2);
    const qreal oy = g.t1 / 2;
    const QPointF tl(g.x - ox, g.y - oy), br(g.x + g.w + ox, g.y + g.h + oy);
    const QPointF bl(g.x - ox, g.y + g.h + oy), tr(g.x + g.w + ox, g.y - oy);
    if (cp == 0x2571 || cp == 0x2573) p.drawLine(bl, tr);
    if (cp == 0x2572 || cp == 0x2573) p.drawLine(tl, br);
}

void paintBlock(QPainter &p, const Grid &g, char32_t cp, const QColor &c)
{
    const qreal right = g.x + g.w;
    const qreal bottom = g.y + g.h;
    auto lower = [&](int eighths) { fillRect(p, g.x, g.snap(bottom - g.h * eighths / 8), right, bottom, c); };
    auto leftPart = [&](int eighths) { fillRect(p, g.x, g.y, g.snap(g.x + g.w * eighths / 8), bottom, c); };
    const qreal mx = g.midX();
    const qreal my = g.midY();
    auto quad = [&](bool ul, bool ur, bool ll, bool lr) {
        if (ul) fillRect(p, g.x, g.y, mx, my, c);
        if (ur) fillRect(p, mx, g.y, right, my, c);
        if (ll) fillRect(p, g.x, my, mx, bottom, c);
        if (lr) fillRect(p, mx, my, right, bottom, c);
    };
    auto shade = [&](int alpha) {
        QColor s = c;
        s.setAlpha(alpha);
        fillRect(p, g.x, g.y, right, bottom, s);
    };

    switch (cp) {
    case 0x2580: fillRect(p, g.x, g.y, right, my, c); break;
    case 0x2581: case 0x2582: case 0x2583: case 0x2584:
    case 0x2585: case 0x2586: case 0x2587: lower(static_cast<int>(cp - 0x2580)); break;
    case 0x2588: fillRect(p, g.x, g.y, right, bottom, c); break;
    case 0x2589: case 0x258A: case 0x258B: case 0x258C:
    case 0x258D: case 0x258E: case 0x258F: leftPart(static_cast<int>(0x2590 - cp)); break;
    case 0x2590: fillRect(p, mx, g.y, right, bottom, c); break;
    case 0x2591: shade(64); break;
    case 0x2592: shade(128); break;
    case 0x2593: shade(192); break;
    case 0x2594: fillRect(p, g.x, g.y, right, g.snap(g.y + g.h / 8), c); break;
    case 0x2595: fillRect(p, g.snap(right - g.w / 8), g.y, right, bottom, c); break;
    case 0x2596: quad(false, false, true,  false); break;
    case 0x2597: quad(false, false, false, true);  break;
    case 0x2598: quad(true,  false, false, false); break;
    case 0x2599: quad(true,  false, true,  true);  break;
    case 0x259A: quad(true,  false, false, true);  break;
    case 0x259B: quad(true,  true,  true,  false); break;
    case 0x259C: quad(true,  true,  false, true);  break;
    case 0x259D: quad(false, true,  false, false); break;
    case 0x259E: quad(false, true,  true,  false); break;
    case 0x259F: quad(false, true,  true,  true);  break;
    default: break;
    }
}

void paintPowerline(QPainter &p, const Grid &g, char32_t cp, const QColor &c)
{
    const qreal right = g.x + g.w;
    const qreal bottom = g.y + g.h;
    const qreal my = g.y + g.h / 2;
    p.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath path;
    switch (cp) {
    case 0xE0B0: // solid right-pointing triangle
        path.moveTo(g.x, g.y);
        path.lineTo(right, my);
        path.lineTo(g.x, bottom);
        path.closeSubpath();
        p.fillPath(path, c);
        return;
    case 0xE0B2: // solid left-pointing triangle
        path.moveTo(right, g.y);
        path.lineTo(g.x, my);
        path.lineTo(right, bottom);
        path.closeSubpath();
        p.fillPath(path, c);
        return;
    case 0xE0B4: // solid right half circle
        path.moveTo(g.x, g.y);
        path.arcTo(QRectF(g.x - g.w, g.y, 2 * g.w, g.h), 90, -180);
        path.closeSubpath();
        p.fillPath(path, c);
        return;
    case 0xE0B6: // solid left half circle
        path.moveTo(right, g.y);
        path.arcTo(QRectF(g.x, g.y, 2 * g.w, g.h), 90, 180);
        path.closeSubpath();
        p.fillPath(path, c);
        return;
    case 0xE0B1: // thin right chevron
        path.moveTo(g.x, g.y);
        path.lineTo(right, my);
        path.lineTo(g.x, bottom);
        break;
    case 0xE0B3: // thin left chevron
        path.moveTo(right, g.y);
        path.lineTo(g.x, my);
        path.lineTo(right, bottom);
        break;
    case 0xE0B5: // thin right half circle
        path.moveTo(g.x, g.y);
        path.arcTo(QRectF(g.x - g.w, g.y, 2 * g.w, g.h), 90, -180);
        break;
    default:     // 0xE0B7 thin left half circle
        path.moveTo(right, g.y);
        path.arcTo(QRectF(g.x, g.y, 2 * g.w, g.h), 90, 180);
        break;
    }
    p.strokePath(path, QPen(c, g.t1, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
}

} // namespace

bool armsFor(char32_t cp, Arms *out)
{
    if (cp < 0x2500 || cp > 0x257F) return false;
    const uint8_t v = kArms[cp - 0x2500];
    if (v == 0) return false;
    if (out) {
        out->left = v & 3;
        out->right = (v >> 2) & 3;
        out->up = (v >> 4) & 3;
        out->down = (v >> 6) & 3;
    }
    return true;
}

void paint(QPainter &p, const QRectF &cell, char32_t cp, QColor color,
           qreal lightStroke, qreal dpr)
{
    if (dpr <= 0) dpr = 1;
    Grid g;
    g.x = cell.x();
    g.y = cell.y();
    g.w = cell.width();
    g.h = cell.height();
    g.dpr = dpr;
    g.t1 = std::max(1.0 / dpr, std::round(lightStroke * dpr) / dpr);
    g.t2 = g.t1 * 2;

    const bool aa = p.testRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::Antialiasing, false);

    Arms arms;
    if (armsFor(cp, &arms)) {
        paintArms(p, g, arms, color);
    } else if (cp >= 0x2504 && cp <= 0x250B) {
        const int idx = static_cast<int>(cp - 0x2504);
        const bool horizontal = (idx % 4) < 2;
        paintDashes(p, g, horizontal, idx < 4 ? 3 : 4, (idx % 2) ? 2 : 1, color);
    } else if (cp >= 0x254C && cp <= 0x254F) {
        const int idx = static_cast<int>(cp - 0x254C);
        paintDashes(p, g, idx < 2, 2, (idx % 2) ? 2 : 1, color);
    } else if (cp >= 0x256D && cp <= 0x2570) {
        paintArc(p, g, cp, color);
    } else if (cp >= 0x2571 && cp <= 0x2573) {
        paintDiagonal(p, g, cp, color);
    } else if (cp >= 0x2580 && cp <= 0x259F) {
        paintBlock(p, g, cp, color);
    } else if (cp >= 0xE0B0 && cp <= 0xE0B7) {
        paintPowerline(p, g, cp, color);
    }

    p.setRenderHint(QPainter::Antialiasing, aa);
}

} // namespace TerminalBoxDrawing
