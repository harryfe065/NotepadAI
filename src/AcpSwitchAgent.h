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

#ifndef ACP_SWITCH_AGENT_H
#define ACP_SWITCH_AGENT_H

#include <QList>
#include <QString>
#include <QVector>

#include <cstdint>

#include "AcpAgentDefinition.h"
#include "AcpSessionModel.h" // AcpMessage

// Rules for Switch agent (GLOSSARY, ADR 0003): when a session may move to
// another agent, which agents qualify, and whether Claude Code's session file
// still holds the whole conversation. Pure functions.
namespace AcpSwitchAgent {

enum class Block : std::uint8_t {
    None,
    Remote,              // SSH session: not supported yet
    SourceNotClaudeCode, // only Claude Code sessions can move
    Switching,
    TurnRunning,
    GoalActive,
    WaitingForUser,      // question form or permission request
    BackgroundWork,      // background tasks or a Monitor watch
    PromptsQueued,
};

struct SessionState
{
    bool remote = false;
    bool sourceIsClaudeCode = false;
    bool switching = false;
    bool turnRunning = false;
    bool goalActive = false;
    bool waitingForUser = false;
    bool backgroundWork = false;
    int queuedPrompts = 0;
};

// First reason the session cannot switch now; Remote and SourceNotClaudeCode
// come first because no amount of waiting changes them.
Block blockReason(const SessionState &state);

// Menu line for a block (translated in the "AcpSwitchAgent" context).
QString blockText(Block block, int backgroundCount = 0);

bool isClaudeCodeAgent(const AcpAgentDefinition &agent);
// Claude Code agents among `agents`, in the same (registry) order.
QList<AcpAgentDefinition> claudeCodeAgents(const QList<AcpAgentDefinition> &agents);

// The agent has been sent at least one prompt (user, goal or host), so it has
// a conversation to resume; host notices alone do not count.
bool hasConversation(const QVector<AcpMessage> &messages);

// Newest prompt / reply / thought the agent itself saw: host notices and the
// goal judge's mirrored turns never reach Claude Code's file. -1 when none.
qint64 newestConversationMs(const QVector<AcpMessage> &messages);

// The file's newest record is no older than the conversation (2 s of slack
// for the host stamping a message before the CLI writes it). A missing file
// timestamp is never current; a session with no messages always is.
bool transcriptIsCurrent(qint64 fileNewestMs, qint64 conversationNewestMs);

} // namespace AcpSwitchAgent

#endif // ACP_SWITCH_AGENT_H
