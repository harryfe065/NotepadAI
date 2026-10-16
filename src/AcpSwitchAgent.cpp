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

#include "AcpSwitchAgent.h"

#include "AcpAgentBrand.h"

#include <QCoreApplication>

#include <algorithm>

namespace AcpSwitchAgent {

namespace {
constexpr qint64 kClockSlackMs = 2000;
}

Block blockReason(const SessionState &state)
{
    if (state.remote)
        return Block::Remote;
    if (!state.sourceIsClaudeCode)
        return Block::SourceNotClaudeCode;
    if (state.switching)
        return Block::Switching;
    if (state.turnRunning)
        return Block::TurnRunning;
    if (state.goalActive)
        return Block::GoalActive;
    if (state.waitingForUser)
        return Block::WaitingForUser;
    if (state.backgroundWork)
        return Block::BackgroundWork;
    if (state.queuedPrompts > 0)
        return Block::PromptsQueued;
    return Block::None;
}

QString blockText(Block block, int backgroundCount)
{
    switch (block) {
    case Block::None:
        return {};
    case Block::Remote:
        return QCoreApplication::translate("AcpSwitchAgent", "Not available for SSH sessions yet");
    case Block::SourceNotClaudeCode:
        return QCoreApplication::translate("AcpSwitchAgent", "No agent can continue this session");
    case Block::Switching:
        return QCoreApplication::translate("AcpSwitchAgent", "Switching agents…");
    case Block::TurnRunning:
        return QCoreApplication::translate("AcpSwitchAgent", "A turn is running");
    case Block::GoalActive:
        return QCoreApplication::translate("AcpSwitchAgent", "A goal is running");
    case Block::WaitingForUser:
        return QCoreApplication::translate("AcpSwitchAgent", "Waiting for your answer");
    case Block::BackgroundWork:
        return backgroundCount > 0
            ? QCoreApplication::translate("AcpSwitchAgent", "%n background task(s) still running", nullptr,
                                          backgroundCount)
            : QCoreApplication::translate("AcpSwitchAgent", "Background work is still running");
    case Block::PromptsQueued:
        return QCoreApplication::translate("AcpSwitchAgent", "Prompts are queued");
    }
    return {};
}

bool isClaudeCodeAgent(const AcpAgentDefinition &agent)
{
    // Detected from the definition, never the chosen icon: a custom icon does
    // not change what the agent can resume.
    return detectAcpAgentBrand(agent) == QLatin1String("claude-code");
}

QList<AcpAgentDefinition> claudeCodeAgents(const QList<AcpAgentDefinition> &agents)
{
    QList<AcpAgentDefinition> out;
    for (const AcpAgentDefinition &a : agents) {
        if (isClaudeCodeAgent(a))
            out.append(a);
    }
    return out;
}

bool hasConversation(const QVector<AcpMessage> &messages)
{
    return std::any_of(messages.cbegin(), messages.cend(), [](const AcpMessage &m) {
        return m.role == QLatin1String("user") && !m.fromGoalJudge;
    });
}

qint64 newestConversationMs(const QVector<AcpMessage> &messages)
{
    qint64 newest = -1;
    for (const AcpMessage &m : messages) {
        if (m.fromGoalJudge)
            continue;
        if (m.role == QLatin1String("user") || m.role == QLatin1String("assistant")
            || m.role == QLatin1String("thought")) {
            newest = qMax(newest, m.timestamp);
        }
    }
    return newest;
}

bool transcriptIsCurrent(qint64 fileNewestMs, qint64 conversationNewestMs)
{
    if (conversationNewestMs < 0)
        return true;
    if (fileNewestMs < 0)
        return false;
    return fileNewestMs >= conversationNewestMs - kClockSlackMs;
}

} // namespace AcpSwitchAgent
