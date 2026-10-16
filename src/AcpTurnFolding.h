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

#ifndef ACP_TURN_FOLDING_H
#define ACP_TURN_FOLDING_H

#include <QHash>
#include <QString>
#include <QVector>

#include "AcpProtocol.h"
#include "AcpSessionModel.h"

// View-side "Worked for Xs" folding. The model/history is never mutated — the
// view uses this to decide which timeline entries of a finished turn are not
// instantiated as widgets (collapsed) until the user expands the turn.
//
// A turn runs from a user/goal anchor to the next anchor (index 0 starts a
// turn too, so an agent-only prefix is its own turn). Per turn:
//   - pinned entries (user / system) always stay in the transcript
//   - the survivor is the LAST assistant message with visible content — the
//     report the user reads; it stays visible beside the fold row
//   - every other entry that would render (tool calls, thoughts, earlier
//     assistant messages) is folded; a turn may fold nothing (report only)
// While the newest turn runs it is not folded (the view shows a live ticking
// "Worked for Xs" row above it); once it ends it folds exactly like every
// older turn.
//
// Complexity: O(n) time over the timeline, one pass.
namespace AcpTurnFolding {

struct Turn
{
    int begin = 0;            // first timeline index of the turn (the anchor when anchor >= 0)
    int end = 0;              // one past the last timeline index
    int anchor = -1;          // timeline index of the user/goal message; -1 for a prefix turn
    int survivor = -1;        // timeline index of the final report; -1 when there is none
    QVector<int> folded;      // timeline indices hidden while collapsed, in order
    int toolCalls = 0;        // tool-call entries among `folded`
    qint64 workedMs = -1;     // anchor -> survivor wall time; -1 when unknown
};

// False when the message would render as an empty bubble: whitespace only, or,
// for an assistant reply, nothing but <br> tags and HTML comments.
bool messageHasVisibleContent(const AcpMessage &msg);

// Every foldable turn except the newest turn (included when includeNewest is
// set, i.e. the newest turn has ended). A turn with nothing to fold is kept only
// when its duration is known (the row is then a plain, non-expandable label).
QVector<Turn> compute(const QVector<AcpTimelineEntry> &timeline,
                      const QVector<AcpMessage> &messages,
                      const QHash<QString, AcpProtocol::AcpToolCall> &toolCalls,
                      bool includeNewest = false);

// First timeline index of the newest turn (last user/goal prompt, else 0);
// -1 for an empty timeline.
int newestTurnBegin(const QVector<AcpTimelineEntry> &timeline,
                    const QVector<AcpMessage> &messages);

} // namespace AcpTurnFolding

#endif // ACP_TURN_FOLDING_H
