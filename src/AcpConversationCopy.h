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

#ifndef ACP_CONVERSATION_COPY_H
#define ACP_CONVERSATION_COPY_H

#include <QString>
#include <QVector>

#include "AcpSessionModel.h"

// "Copy response" / "Copy conversation" under an assistant bubble. Both walk
// the timeline (not the message vector: a finished goal-judge turn leaves its
// messages behind with no timeline entry).
//
// A turn runs from a user prompt to the next one (an agent-only prefix is a
// turn too). Its final report is its LAST assistant message with visible text;
// goal-judge output is never a report. Thoughts, tool calls, system lines and
// interim replies are not part of the conversation.
//
// Complexity: O(n) over the timeline, one pass, no per-entry allocation beyond
// the output.
namespace AcpConversationCopy {

// Visible text of a message: text blocks joined, images as "[image]".
QString messageText(const AcpMessage &msg);

// Message indices of each turn's final report, in timeline order. The newest
// turn counts only when includeNewest is set (it has ended).
QVector<int> finalReports(const QVector<AcpTimelineEntry> &timeline,
                          const QVector<AcpMessage> &messages,
                          bool includeNewest);

// Markdown transcript from the start through the timeline entry of message
// upToMessage (inclusive; its turn counts as ended): every prompt under
// "## User" and each turn's report under "## Assistant". Empty when that
// message is not on the timeline.
QString buildMarkdown(const QVector<AcpTimelineEntry> &timeline,
                      const QVector<AcpMessage> &messages,
                      int upToMessage);

} // namespace AcpConversationCopy

#endif // ACP_CONVERSATION_COPY_H
