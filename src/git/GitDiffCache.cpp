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

#include "GitDiffCache.h"

#include <xxhash.h>

GitDiffCache::GitDiffCache(qsizetype capacityBytes)
    : m_capacity(capacityBytes)
{
    m_map.reserve(128);
}

GitDiffCache::Key GitDiffCache::keyFor(const QString &repo, const QString &relPath, bool stagedSide)
{
    // Build a small contiguous buffer and hash once with XXH3 (fast 64-bit).
    QByteArray buf;
    buf.reserve(repo.size() + relPath.size() + 4);
    buf.append(repo.toUtf8());
    buf.append('\0');
    buf.append(relPath.toUtf8());
    buf.append('\0');
    buf.append(stagedSide ? '1' : '0');
    return XXH3_64bits(buf.constData(), static_cast<size_t>(buf.size()));
}

void GitDiffCache::touch(Key k)
{
    m_order.removeOne(k);
    m_order.append(k);
}

GitDiffCache::Entry GitDiffCache::get(Key k)
{
    const auto it = m_map.constFind(k);
    if (it == m_map.constEnd()) return {};
    touch(k);
    return it->value;
}

void GitDiffCache::put(Key k, Entry value, qsizetype rawDiffBytes)
{
    if (!value) return;
    // Estimate footprint: parsed.texts byte total + small per-row overhead.
    // We approximate with raw diff bytes + a 1.3x fudge for QVector headers.
    // The built document holds its text and one style byte per text byte,
    // plus ~12 bytes of row tables per row.
    qsizetype footprint = (rawDiffBytes * 13) / 10;
    if (value.doc)
        footprint += 2 * value.doc->text.size() + 12 * static_cast<qsizetype>(value.doc->rowCount());

    if (const auto it = m_map.find(k); it != m_map.end()) {
        m_sizeBytes -= it->bytes;
        it->value = value;
        it->bytes = footprint;
        m_sizeBytes += footprint;
        touch(k);
    } else {
        m_map.insert(k, Slot{ std::move(value), footprint });
        m_order.append(k);
        m_sizeBytes += footprint;
    }
    evictWhileOver();
}

void GitDiffCache::evictWhileOver()
{
    while (m_sizeBytes > m_capacity && !m_order.isEmpty()) {
        const Key victim = m_order.takeFirst();
        const auto it = m_map.find(victim);
        if (it == m_map.end()) continue;
        m_sizeBytes -= it->bytes;
        m_map.erase(it);
    }
}

void GitDiffCache::clear()
{
    m_map.clear();
    m_order.clear();
    m_sizeBytes = 0;
}
