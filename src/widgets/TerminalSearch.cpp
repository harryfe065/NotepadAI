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

#include "TerminalSearch.h"

namespace {
constexpr uint32_t kWideSpacer = 0xFFFFFFFFu;
} // namespace

QRegularExpression terminalSearchRegex(const TerminalSearchOptions &options)
{
    if (options.query.isEmpty()) {
        return QRegularExpression(QStringLiteral("("));  // deliberately invalid
    }
    QString pattern = options.regex ? options.query : QRegularExpression::escape(options.query);
    if (options.wholeWord) {
        pattern = QStringLiteral("\\b(?:") + pattern + QStringLiteral(")\\b");
    }
    QRegularExpression::PatternOptions flags = QRegularExpression::UseUnicodePropertiesOption;
    if (!options.caseSensitive) {
        flags |= QRegularExpression::CaseInsensitiveOption;
    }
    QRegularExpression re(pattern, flags);
    re.optimize();
    return re;
}

void terminalRowText(const VTermScreenCell *cells, int cols, QString &text, std::vector<int> &colOf)
{
    text.clear();
    colOf.clear();
    for (int col = 0; col < cols; ++col) {
        const VTermScreenCell &cell = cells[col];
        const uint32_t ch = cell.chars[0];
        if (ch == kWideSpacer) continue;
        if (ch == 0) {
            text.append(QLatin1Char(' '));
            colOf.push_back(col);
            continue;
        }
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i) {
            const char32_t cp = cell.chars[i];
            if (QChar::requiresSurrogates(cp)) {
                text.append(QChar(QChar::highSurrogate(cp)));
                text.append(QChar(QChar::lowSurrogate(cp)));
                colOf.push_back(col);
                colOf.push_back(col);
            } else {
                text.append(QChar(static_cast<char16_t>(cp)));
                colOf.push_back(col);
            }
        }
    }
}

void searchTerminalRow(const QRegularExpression &re, qint64 line, const VTermScreenCell *cells, int cols,
                       std::vector<TerminalSearchMatch> &out, QString &text, std::vector<int> &colOf)
{
    terminalRowText(cells, cols, text, colOf);
    // Rows that are all blank cannot match a non-blank query; skip the engine.
    bool blank = true;
    for (const QChar c : text) {
        if (c != QLatin1Char(' ')) { blank = false; break; }
    }
    if (blank) return;

    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const qsizetype start = m.capturedStart();
        const qsizetype end = m.capturedEnd();
        if (end <= start) continue;  // empty matches carry nothing to show
        const int colStart = colOf[static_cast<std::size_t>(start)];
        const int lastCol = colOf[static_cast<std::size_t>(end - 1)];
        const int lastWidth = cells[lastCol].width >= 2 ? 2 : 1;
        out.push_back({line, colStart, lastCol + lastWidth - colStart});
    }
}

std::vector<TerminalSearchMatch> searchScrollback(const ScrollbackBuffer &buffer, qint64 firstLineId,
                                                  int fromIndex, int cols, const QRegularExpression &re,
                                                  const std::atomic<bool> *cancel)
{
    std::vector<TerminalSearchMatch> out;
    if (!re.isValid() || cols <= 0) return out;
    std::vector<VTermScreenCell> row(static_cast<std::size_t>(cols));
    QString text;
    std::vector<int> colOf;
    for (int i = std::max(0, fromIndex); i < buffer.size(); ++i) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            out.clear();
            return out;
        }
        if (buffer.lineAt(i, cols, row.data())) {
            searchTerminalRow(re, firstLineId + i, row.data(), cols, out, text, colOf);
        }
    }
    return out;
}
