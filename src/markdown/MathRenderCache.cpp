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

#include "MathRenderCache.h"

#include <cmath>

namespace Math {

size_t qHash(const RenderCache::Key &k, size_t seed)
{
    return qHashMulti(seed, k.source, k.display, k.pxQ, quint32(k.rgba), k.dprQ);
}

RenderCache::RenderCache(int maxEntries, qint64 maxBytes)
    : m_maxEntries(maxEntries > 0 ? maxEntries : 1)
    , m_maxBytes(maxBytes > 0 ? maxBytes : 1)
{
}

RenderCache &RenderCache::instance()
{
    static RenderCache cache;
    return cache;
}

RenderCache::Key RenderCache::makeKey(const QString &source, bool display, double px,
                                      const QColor &color, double dpr)
{
    Key k;
    k.source = source;
    k.display = display;
    k.pxQ = quint32(std::lround(px * 16.0));
    k.rgba = color.rgba();
    k.dprQ = quint32(std::lround(dpr * 100.0));
    return k;
}

bool RenderCache::contains(const QString &source, bool display, double px,
                           const QColor &color, double dpr) const
{
    return m_index.contains(makeKey(source, display, px, color, dpr));
}

std::optional<Rendered> RenderCache::get(const QString &source, bool display, double px,
                                         const QColor &color, double dpr)
{
    const Key key = makeKey(source, display, px, color, dpr);
    const auto found = m_index.constFind(key);
    if (found != m_index.constEnd()) {
        ++m_hits;
        m_lru.splice(m_lru.begin(), m_lru, found.value()); // iterator stays valid
        return m_lru.front().value;
    }

    ++m_misses;
    Entry e;
    e.key = key;
    e.value = renderFormula(source, display, px, color, dpr);
    // Negative entries still cost a little (key string + node); charge a floor.
    e.bytes = e.value ? qint64(e.value->image.sizeInBytes()) + source.size() * 2 + 64
                      : source.size() * 2 + 64;
    m_bytes += e.bytes;
    m_lru.push_front(std::move(e));
    m_index.insert(key, m_lru.begin());
    evict();
    return m_lru.empty() ? std::nullopt : m_lru.front().value;
}

void RenderCache::evict()
{
    // Never evict the entry just inserted (front): a single oversized formula
    // is still returned to the caller, it simply is the only resident entry.
    while (m_lru.size() > 1
           && (int(m_lru.size()) > m_maxEntries || m_bytes > m_maxBytes)) {
        const Entry &victim = m_lru.back();
        m_bytes -= victim.bytes;
        m_index.remove(victim.key);
        m_lru.pop_back();
    }
}

void RenderCache::clear()
{
    m_lru.clear();
    m_index.clear();
    m_bytes = 0;
}

} // namespace Math
