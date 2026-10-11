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

#ifndef MATH_RENDER_CACHE_H
#define MATH_RENDER_CACHE_H

#include <QColor>
#include <QHash>
#include <QString>

#include <list>
#include <optional>

#include "MathLayout.h"

// LRU cache of rendered formulas. Assistant messages re-render on every
// streamed chunk, so a formula already drawn must cost one hash lookup.
// Unsupported formulas are cached too (negative entries): they are re-parsed
// neither per chunk nor per theme-unchanged re-render.
//
// Key: source + display + pixel size + color + device pixel ratio, so a palette
// or DPI change simply misses and re-renders. UI-thread only (painting needs
// the GUI thread); no locking.
//
// Complexity: get() is O(1) expected on a hit; a miss adds one layout + paint.
namespace Math {

class RenderCache
{
public:
    explicit RenderCache(int maxEntries = 256, qint64 maxBytes = 8 * 1024 * 1024);

    // Process-wide cache used by the chat widgets.
    static RenderCache &instance();

    // Cached or freshly rendered; nullopt when the formula is unsupported.
    std::optional<Rendered> get(const QString &source, bool display, double px,
                                const QColor &color, double dpr);

    bool contains(const QString &source, bool display, double px,
                  const QColor &color, double dpr) const;

    int size() const { return int(m_lru.size()); }
    qint64 bytes() const { return m_bytes; }
    int hits() const { return m_hits; }
    int misses() const { return m_misses; }
    void clear();

private:
    struct Key
    {
        QString source;
        bool display = false;
        quint32 pxQ = 0;   // px * 16, rounded
        QRgb rgba = 0;
        quint32 dprQ = 0;  // dpr * 100, rounded
        bool operator==(const Key &o) const
        {
            return display == o.display && pxQ == o.pxQ && rgba == o.rgba && dprQ == o.dprQ
                   && source == o.source;
        }
    };
    friend size_t qHash(const Key &k, size_t seed);

    struct Entry
    {
        Key key;
        std::optional<Rendered> value;
        qint64 bytes = 0;
    };
    using List = std::list<Entry>;

    static Key makeKey(const QString &source, bool display, double px, const QColor &color, double dpr);
    void evict();

    int m_maxEntries;
    qint64 m_maxBytes;
    qint64 m_bytes = 0;
    int m_hits = 0;
    int m_misses = 0;
    List m_lru; // front = most recently used
    QHash<Key, List::iterator> m_index;
};

} // namespace Math

#endif // MATH_RENDER_CACHE_H
