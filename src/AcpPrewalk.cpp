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

#include "AcpPrewalk.h"

AcpPrewalk::ToolCall AcpPrewalk::classify(const AcpProtocol::AcpToolCall &tc, bool fromGoalJudge)
{
    ToolCall call;
    call.editsFiles = tc.kind.isEmpty() ? AcpProtocol::toolCallLooksFileMutating(tc)
                                        : AcpProtocol::toolCallKindLooksFileMutating(tc.kind);
    call.fromSubagent = !tc.parentToolCallId.isEmpty();
    call.fromGoalJudge = fromGoalJudge;
    return call;
}

QString AcpPrewalk::onToolCall(const ToolCall &call, const Selection &current)
{
    if (!m_inTurn || !call.editsFiles || call.fromSubagent || call.fromGoalJudge)
        return {};
    switch (m_phase) {
    case Phase::Switched:
        return {};
    case Phase::Restoring:
        // The restore has not landed: the session selection is still the one
        // saved at the switch, and the agent may already run the target.
        if (m_target.isEmpty())
            return {};
        m_phase = Phase::Switched;
        return m_target;
    case Phase::Idle:
        if (m_target.isEmpty() || current.model.isEmpty() || current.model == m_target)
            return {};
        m_session = current;
        m_phase = Phase::Switched;
        return m_target;
    }
    return {};
}

std::optional<AcpPrewalk::Selection> AcpPrewalk::onTurnEnded()
{
    m_inTurn = false;
    if (m_phase != Phase::Switched)
        return std::nullopt;
    m_phase = Phase::Restoring;
    return m_session;
}

QString AcpPrewalk::onSelectionReported(const Selection &current)
{
    if (m_phase != Phase::Restoring || current.model != m_session.model)
        return {};
    const QString effort = !m_session.effort.isEmpty() && current.effort != m_session.effort
        ? m_session.effort
        : QString();
    m_phase = Phase::Idle;
    m_session = {};
    return effort;
}

void AcpPrewalk::onUserPickedModel()
{
    m_phase = Phase::Idle;
    m_session = {};
}

void AcpPrewalk::onUserPickedEffort(const QString &effort)
{
    if (m_phase != Phase::Idle)
        m_session.effort = effort;
}

void AcpPrewalk::reset()
{
    m_phase = Phase::Idle;
    m_session = {};
    m_inTurn = false;
}

std::optional<AcpPrewalk::Selection> AcpPrewalk::sessionSelection() const
{
    if (m_phase == Phase::Idle)
        return std::nullopt;
    return m_session;
}
