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

#include "TerminalContrast.h"

#include <cmath>

namespace {
constexpr int kMaxCachedPairs = 4096;
} // namespace

namespace TerminalContrast {

namespace {

constexpr int kSearchSteps = 12;

double screenLuminance(QRgb c)
{
    auto lin = [](int v) { return std::pow(v / 255.0, 2.4); };
    double y = 0.2126729 * lin(qRed(c)) + 0.7151522 * lin(qGreen(c)) + 0.0721750 * lin(qBlue(c));
    // Soft clamp near black.
    if (y < 0.022) {
        y += std::pow(0.022 - y, 1.414);
    }
    return y;
}

QRgb mix(QRgb from, QRgb to, double t)
{
    auto ch = [t](int a, int b) { return static_cast<int>(std::lround(a + (b - a) * t)); };
    return qRgb(ch(qRed(from), qRed(to)), ch(qGreen(from), qGreen(to)), ch(qBlue(from), qBlue(to)));
}

// Smallest blend toward `target` that reaches the floor, or the full target
// when even that falls short.
QRgb searchToward(QRgb fg, QRgb bg, QRgb target, double minLc, bool *reached)
{
    if (std::abs(apca(target, bg)) < minLc) {
        *reached = false;
        return target;
    }
    *reached = true;
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < kSearchSteps; ++i) {
        const double mid = (lo + hi) / 2;
        if (std::abs(apca(mix(fg, target, mid), bg)) >= minLc) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return mix(fg, target, hi);
}

} // namespace

double apca(QRgb text, QRgb bg)
{
    const double yt = screenLuminance(text);
    const double yb = screenLuminance(bg);
    if (std::abs(yb - yt) < 0.0005) {
        return 0.0;
    }
    if (yb > yt) {
        const double s = (std::pow(yb, 0.56) - std::pow(yt, 0.57)) * 1.14;
        return s < 0.1 ? 0.0 : (s - 0.027) * 100.0;
    }
    const double s = (std::pow(yb, 0.65) - std::pow(yt, 0.62)) * 1.14;
    return s > -0.1 ? 0.0 : (s + 0.027) * 100.0;
}

QRgb ensureMinimum(QRgb fg, QRgb bg, double minLc)
{
    if (minLc <= 0 || std::abs(apca(fg, bg)) >= minLc) {
        return fg;
    }
    // Move away from the background first: darker text on light backgrounds.
    const bool lightBg = screenLuminance(bg) > 0.18;
    const QRgb first = lightBg ? qRgb(0, 0, 0) : qRgb(255, 255, 255);
    const QRgb second = lightBg ? qRgb(255, 255, 255) : qRgb(0, 0, 0);
    bool reached = false;
    const QRgb a = searchToward(fg, bg, first, minLc, &reached);
    if (reached) {
        return a;
    }
    const QRgb b = searchToward(fg, bg, second, minLc, &reached);
    if (reached) {
        return b;
    }
    return std::abs(apca(a, bg)) >= std::abs(apca(b, bg)) ? a : b;
}

} // namespace TerminalContrast

void TerminalContrastCache::setMinimum(double minLc)
{
    if (minLc == m_min) return;
    m_min = minLc;
    m_cache.clear();
}

QRgb TerminalContrastCache::adjust(QRgb fg, QRgb bg)
{
    if (m_min <= 0) return fg;
    const quint64 key = (static_cast<quint64>(fg & 0xFFFFFFu) << 24) | (bg & 0xFFFFFFu);
    const auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) {
        return it.value();
    }
    const QRgb out = TerminalContrast::ensureMinimum(fg, bg, m_min);
    if (m_cache.size() >= kMaxCachedPairs) {
        m_cache.clear();
    }
    m_cache.insert(key, out);
    return out;
}
