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
//     assistant messages) is folded
// The LAST turn is never folded: the user reviews it; it folds once a newer
// prompt starts a new turn.
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
    qint64 workedMs = -1;     // anchor -> survivor wall time; -1 when unknown
};

bool messageHasVisibleContent(const AcpMessage &msg);

// Every foldable turn except the newest turn. Turns with nothing to fold are omitted.
QVector<Turn> compute(const QVector<AcpTimelineEntry> &timeline,
                      const QVector<AcpMessage> &messages,
                      const QHash<QString, AcpProtocol::AcpToolCall> &toolCalls);

} // namespace AcpTurnFolding

#endif // ACP_TURN_FOLDING_H
