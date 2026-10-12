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

#ifndef TERMINALHYPERLINKS_H
#define TERMINALHYPERLINKS_H

#include <QString>

#include <cstddef>
#include <deque>
#include <functional>
#include <vector>

// The OSC 8 hyperlinks of one terminal, in absolute line ids so they scroll
// with their text. A span stays a link only while its cells still hold the
// text it covered: output that overwrites it (a clear, a redraw) kills it for
// good. That is re-checked once after output reaches its rows (markStale),
// never per paint.
class TerminalHyperlinks
{
public:
    struct Span {
        qint64 startLine = 0;
        int startCol = 0;
        qint64 endLine = 0;
        int endCol = 0;   // exclusive
        QString uri;
        QString text;     // what it covered
        bool dead = false;
        bool stale = false;
    };
    // The text now in [start, end), or a null string when part of it left
    // history.
    using TextReader = std::function<QString(qint64 startLine, int startCol, qint64 endLine, int endCol)>;

    static constexpr std::size_t kMaxSpans = 2048;   // oldest dropped first

    // Schemes that would launch arbitrary OS handlers are never links.
    static bool isAllowed(const QString &uri);

    // OSC 8 ; params ; URI with the cursor at (line, col): an allowed URI
    // opens a link, anything else (an empty URI) ends the open one. Returns
    // the span recorded by closing, if any.
    const Span *handle(const QString &uri, qint64 line, int col, const TextReader &text);
    // Output reached lines [fromLine, toLine).
    void markStale(qint64 fromLine, qint64 toLine);
    // The screen buffer swapped: links from `line` on describe nothing shown.
    void killFrom(qint64 line);
    // Lines before `firstLine` left history.
    void dropBefore(qint64 firstLine);
    // The live link covering a cell.
    const Span *at(qint64 line, int col, const TextReader &text);

    // Spans reaching lines [firstLine, lastLine], gathered once per paint;
    // forEachOnLine then visits the live ones on one of those lines with the
    // columns they cover ([from, to), to = -1 up to the end of the row).
    void gatherVisible(qint64 firstLine, qint64 lastLine);
    template <typename Visit>
    void forEachOnLine(qint64 line, const TextReader &text, Visit &&visit)
    {
        for (const std::size_t index : m_visible) {
            Span &span = m_spans[index];
            if (span.endLine < line || span.startLine > line || !alive(span, text)) continue;
            visit(span.startLine == line ? span.startCol : 0, span.endLine == line ? span.endCol : -1);
        }
    }

    bool isEmpty() const { return m_spans.empty(); }
    const std::deque<Span> &spans() const { return m_spans; }

private:
    bool alive(Span &span, const TextReader &text);

    std::deque<Span> m_spans;
    std::vector<std::size_t> m_visible;
    bool m_open = false;
    qint64 m_startLine = 0;
    int m_startCol = 0;
    QString m_uri;
};

#endif
