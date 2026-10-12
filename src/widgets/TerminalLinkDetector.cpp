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

#include "TerminalLinkDetector.h"

#include <QRegularExpression>

namespace {

bool isBoxDrawing(QChar c)
{
    return c.unicode() >= 0x2500 && c.unicode() <= 0x259F;
}

bool isTokenBreak(QChar c)
{
    return c.isSpace() || c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`')
        || c == QLatin1Char('<') || c == QLatin1Char('>') || c == QLatin1Char('|') || isBoxDrawing(c);
}

bool isTrailingPunct(QChar c)
{
    return c == QLatin1Char('.') || c == QLatin1Char(',') || c == QLatin1Char(';')
        || c == QLatin1Char(':') || c == QLatin1Char('!') || c == QLatin1Char('?');
}

// Drops a closing bracket that has no opener inside the span: "(see x.cpp)".
bool trimUnbalancedClose(const QString &text, int start, int *end)
{
    const QChar last = text.at(*end - 1);
    QChar open;
    if (last == QLatin1Char(')')) open = QLatin1Char('(');
    else if (last == QLatin1Char(']')) open = QLatin1Char('[');
    else if (last == QLatin1Char('}')) open = QLatin1Char('{');
    else return false;
    int depth = 0;
    for (int i = start; i < *end; ++i) {
        if (text.at(i) == open) ++depth;
        else if (text.at(i) == last) --depth;
    }
    if (depth < 0) {
        --*end;
        return true;
    }
    return false;
}

void trimSpan(const QString &text, int start, int *end)
{
    bool changed = true;
    while (changed && *end > start) {
        changed = false;
        if (isTrailingPunct(text.at(*end - 1))) {
            --*end;
            changed = true;
        } else if (trimUnbalancedClose(text, start, end)) {
            changed = true;
        }
    }
}

TerminalLink urlAt(const QString &text, int index)
{
    static const QRegularExpression re(QStringLiteral(
        "\\b(?:https?|ftp|file|mailto|ssh|git|ipfs|ipns|magnet|gemini|gopher|news):"
        "[^\\s<>\"'`\\x{2500}-\\x{259F}]+"));
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int start = static_cast<int>(m.capturedStart());
        int end = static_cast<int>(m.capturedEnd());
        if (start > index) break;
        trimSpan(text, start, &end);
        if (index >= start && index < end && end - start > 6) {
            TerminalLink link;
            link.kind = TerminalLink::Url;
            link.start = start;
            link.length = end - start;
            link.target = text.mid(start, end - start);
            return link;
        }
    }
    return {};
}

TerminalLink pythonTracebackAt(const QString &text, int index)
{
    static const QRegularExpression re(QStringLiteral("File \"(?<path>[^\"]+)\", line (?<line>\\d+)"));
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int start = static_cast<int>(m.capturedStart(QStringLiteral("path")));
        const int end = static_cast<int>(m.capturedEnd(QStringLiteral("path")));
        if (index >= start && index < end) {
            TerminalLink link;
            link.kind = TerminalLink::Path;
            link.start = start;
            link.length = end - start;
            link.target = m.captured(QStringLiteral("path"));
            link.line = m.captured(QStringLiteral("line")).toInt();
            return link;
        }
    }
    return {};
}

bool looksLikePath(const QString &path)
{
    if (path.isEmpty()) return false;
    if (path.contains(QLatin1Char('/')) || path.contains(QLatin1Char('\\'))) {
        // Not just separators or a bare "//" comment marker.
        for (const QChar c : path) {
            if (c.isLetterOrNumber()) return true;
        }
        return false;
    }
    static const QRegularExpression ext(QStringLiteral("^[\\w.-]*\\w\\.[A-Za-z][\\w-]{0,9}$"));
    return ext.match(path).hasMatch();
}

TerminalLink pathAt(const QString &text, int index)
{
    if (index < 0 || index >= text.size() || isTokenBreak(text.at(index))) return {};
    int start = index;
    while (start > 0 && !isTokenBreak(text.at(start - 1))) --start;
    int end = index + 1;
    while (end < text.size() && !isTokenBreak(text.at(end))) ++end;

    // Leading openers belong to the surrounding prose, not the path.
    while (start < end && (text.at(start) == QLatin1Char('(') || text.at(start) == QLatin1Char('[')
                           || text.at(start) == QLatin1Char('{'))) {
        ++start;
    }
    trimSpan(text, start, &end);
    if (index < start || index >= end) return {};

    static const QRegularExpression re(QStringLiteral(
        "^(?<path>.+?)"
        "(?::(?<l1>\\d+)(?::(?<c1>\\d+))?|\\((?<l2>\\d+)(?:,\\s*(?<c2>\\d+))?\\))?$"));
    const QString token = text.mid(start, end - start);
    const QRegularExpressionMatch m = re.match(token);
    if (!m.hasMatch()) return {};
    const QString path = m.captured(QStringLiteral("path"));
    if (!looksLikePath(path)) return {};

    TerminalLink link;
    link.kind = TerminalLink::Path;
    link.start = start;
    link.length = end - start;
    link.target = path;
    const QString line = m.captured(QStringLiteral("l1")).isEmpty() ? m.captured(QStringLiteral("l2"))
                                                                   : m.captured(QStringLiteral("l1"));
    const QString col = m.captured(QStringLiteral("c1")).isEmpty() ? m.captured(QStringLiteral("c2"))
                                                                  : m.captured(QStringLiteral("c1"));
    link.line = line.toInt();
    link.column = col.toInt();
    return link;
}

} // namespace

TerminalLink findTerminalLinkAt(const QString &text, int index)
{
    if (index < 0 || index >= text.size()) return {};
    TerminalLink link = urlAt(text, index);
    if (link.kind != TerminalLink::None) return link;
    link = pythonTracebackAt(text, index);
    if (link.kind != TerminalLink::None) return link;
    return pathAt(text, index);
}
