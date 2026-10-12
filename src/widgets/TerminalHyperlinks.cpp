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

#include "TerminalHyperlinks.h"

#include <QUrl>

bool TerminalHyperlinks::isAllowed(const QString &uri)
{
    const QString scheme = QUrl(uri).scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto")
        || scheme == QLatin1String("file");
}

const TerminalHyperlinks::Span *TerminalHyperlinks::handle(const QString &uri, qint64 line, int col,
                                                           const TextReader &text)
{
    const Span *recorded = nullptr;
    if (m_open && (line > m_startLine || (line == m_startLine && col > m_startCol))) {
        m_spans.push_back({m_startLine, m_startCol, line, col, m_uri, text(m_startLine, m_startCol, line, col)});
        if (m_spans.size() > kMaxSpans) m_spans.pop_front();
        recorded = &m_spans.back();
    }
    m_open = isAllowed(uri);
    if (m_open) {
        m_startLine = line;
        m_startCol = col;
        m_uri = uri;
    }
    return recorded;
}

void TerminalHyperlinks::markStale(qint64 fromLine, qint64 toLine)
{
    for (Span &span : m_spans) {
        if (span.endLine >= fromLine && span.startLine < toLine) span.stale = true;
    }
}

void TerminalHyperlinks::killFrom(qint64 line)
{
    for (Span &span : m_spans) {
        if (span.endLine >= line) span.dead = true;
    }
}

void TerminalHyperlinks::dropBefore(qint64 firstLine)
{
    while (!m_spans.empty() && m_spans.front().endLine < firstLine) m_spans.pop_front();
}

const TerminalHyperlinks::Span *TerminalHyperlinks::at(qint64 line, int col, const TextReader &text)
{
    for (auto it = m_spans.rbegin(); it != m_spans.rend(); ++it) {
        const bool after = line > it->startLine || (line == it->startLine && col >= it->startCol);
        const bool before = line < it->endLine || (line == it->endLine && col < it->endCol);
        if (after && before && alive(*it, text)) return &*it;
    }
    return nullptr;
}

void TerminalHyperlinks::gatherVisible(qint64 firstLine, qint64 lastLine)
{
    m_visible.clear();
    for (std::size_t i = 0; i < m_spans.size(); ++i) {
        if (m_spans[i].endLine >= firstLine && m_spans[i].startLine <= lastLine) m_visible.push_back(i);
    }
}

bool TerminalHyperlinks::alive(Span &span, const TextReader &text)
{
    if (span.dead) return false;
    if (span.stale) {
        span.stale = false;
        span.dead = text(span.startLine, span.startCol, span.endLine, span.endCol) != span.text;
    }
    return !span.dead;
}
