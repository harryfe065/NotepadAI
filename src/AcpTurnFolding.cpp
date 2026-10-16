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

#include "AcpTurnFolding.h"

#include "AcpTranscriptTruncation.h"

namespace AcpTurnFolding {

namespace {

// Only a real prompt (user or goal-agent user message) starts a foldable turn.
// The goal-judging system marker is a truncation anchor but not a turn here:
// the judge turn is removed from the model once it finishes, and folding the
// working turn when the judge starts would hide it for no reason.
bool isUserAnchor(const AcpTimelineEntry &entry, const QVector<AcpMessage> &messages)
{
    if (entry.kind != AcpTimelineEntry::Kind::Message
        || entry.messageIndex < 0 || entry.messageIndex >= messages.size()) {
        return false;
    }
    return messages.at(entry.messageIndex).role == QLatin1String("user");
}

// True when `s` has a character that shows. With `markdown` (assistant
// bubbles), <br> / <br/> / <br /> and closed <!-- --> comments do not count:
// MarkdownPreprocessor turns them into a line break or drops them, so a reply
// made only of them would render as an empty card. Stops at the first visible
// character; no allocation.
bool hasVisibleText(QStringView s, bool markdown)
{
    const qsizetype n = s.size();
    qsizetype i = 0;
    while (i < n) {
        const QChar c = s[i];
        if (c.isSpace()) {
            ++i;
            continue;
        }
        if (!markdown || c != QLatin1Char('<'))
            return true;
        if (s.mid(i, 4) == QLatin1String("<!--")) {
            const qsizetype e = s.indexOf(QLatin1String("-->"), i + 4);
            if (e < 0)
                return true;
            i = e + 3;
            continue;
        }
        if (n - i < 4 || s.mid(i + 1, 2).compare(QLatin1String("br"), Qt::CaseInsensitive) != 0)
            return true;
        qsizetype j = i + 3;
        while (j < n && (s[j] == QLatin1Char(' ') || s[j] == QLatin1Char('\t')))
            ++j;
        if (j < n && s[j] == QLatin1Char('/'))
            ++j;
        while (j < n && (s[j] == QLatin1Char(' ') || s[j] == QLatin1Char('\t')))
            ++j;
        if (j >= n || s[j] != QLatin1Char('>'))
            return true;
        i = j + 1;
    }
    return false;
}

} // namespace

bool messageHasVisibleContent(const AcpMessage &msg)
{
    const bool markdown = msg.role == QLatin1String("assistant");
    for (const auto &block : msg.content) {
        if (block.kind == AcpProtocol::AcpContentBlock::Kind::Image && !block.imageData.isEmpty())
            return true;
        if (hasVisibleText(block.text, markdown))
            return true;
    }
    return false;
}

QVector<Turn> compute(const QVector<AcpTimelineEntry> &timeline,
                      const QVector<AcpMessage> &messages,
                      const QHash<QString, AcpProtocol::AcpToolCall> &toolCalls,
                      bool includeNewest)
{
    QVector<Turn> out;
    const int n = timeline.size();
    if (n == 0)
        return out;

    // Turn starts: index 0 plus every user/goal prompt.
    QVector<int> starts;
    starts.append(0);
    for (int i = 1; i < n; ++i) {
        if (isUserAnchor(timeline.at(i), messages))
            starts.append(i);
    }
    starts.append(n);
    const int turnCount = starts.size() - 1;

    // The newest turn may still be running; only older ones can fold, unless
    // the caller says it has ended.
    const int lastTurn = includeNewest ? turnCount : turnCount - 1;
    for (int t = 0; t < lastTurn; ++t) {
        Turn turn;
        turn.begin = starts.at(t);
        turn.end = starts.at(t + 1);
        if (isUserAnchor(timeline.at(turn.begin), messages))
            turn.anchor = turn.begin;

        for (int i = turn.end - 1; i >= turn.begin; --i) {
            const AcpTimelineEntry &e = timeline.at(i);
            if (e.kind != AcpTimelineEntry::Kind::Message
                || e.messageIndex < 0 || e.messageIndex >= messages.size()) {
                continue;
            }
            const AcpMessage &m = messages.at(e.messageIndex);
            if (m.role == QLatin1String("assistant") && messageHasVisibleContent(m)) {
                turn.survivor = i;
                break;
            }
        }

        for (int i = turn.begin; i < turn.end; ++i) {
            if (i == turn.survivor || AcpTranscriptTruncation::isPinnedEntry(timeline.at(i), messages))
                continue;
            const AcpTimelineEntry &e = timeline.at(i);
            if (e.kind == AcpTimelineEntry::Kind::ToolCall) {
                // A subagent's call attributed after it got its entry renders
                // inside its Agent card, not here.
                const auto tc = toolCalls.constFind(e.toolCallId);
                if (tc != toolCalls.constEnd() && tc->parentToolCallId.isEmpty()) {
                    turn.folded.append(i);
                    ++turn.toolCalls;
                }
            } else if (e.messageIndex >= 0 && e.messageIndex < messages.size()
                       && messageHasVisibleContent(messages.at(e.messageIndex))) {
                turn.folded.append(i);
            }
        }
        if (turn.anchor >= 0 && turn.survivor >= 0) {
            const qint64 a = messages.at(timeline.at(turn.anchor).messageIndex).timestamp;
            const qint64 b = messages.at(timeline.at(turn.survivor).messageIndex).timestamp;
            if (a > 0 && b >= a)
                turn.workedMs = b - a;
        }
        // A turn with nothing to fold still gets its "Worked for Xs" label
        // (report-only reply), as long as there is a duration to show.
        if (turn.folded.isEmpty() && turn.workedMs < 0)
            continue;
        out.append(turn);
    }
    return out;
}

int newestTurnBegin(const QVector<AcpTimelineEntry> &timeline,
                    const QVector<AcpMessage> &messages)
{
    if (timeline.isEmpty())
        return -1;
    for (int i = timeline.size() - 1; i > 0; --i) {
        if (isUserAnchor(timeline.at(i), messages))
            return i;
    }
    return 0;
}

} // namespace AcpTurnFolding
