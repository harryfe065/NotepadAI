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

#ifndef ACP_PREWALK_H
#define ACP_PREWALK_H

#include <QString>

#include <cstdint>
#include <optional>

#include "AcpProtocol.h"

// Prewalk (GLOSSARY): the session's model reads and plans; at the main agent's
// first file edit of a turn the session moves to the prewalk model, and when
// the turn ends it goes back to its own model and effort. A model the user
// picks meanwhile wins. Pure state: the caller sends the config changes and
// feeds back what the agent reports.
class AcpPrewalk
{
public:
    struct Selection
    {
        QString model;
        QString effort; // empty: the agent has no effort option
    };

    // A tool call as far as Prewalk cares.
    struct ToolCall
    {
        bool editsFiles = false;
        bool fromSubagent = false;
        bool fromGoalJudge = false;
    };

    // A file edit: kind edit / delete / move. Only kindless calls fall back to
    // the name/title needles, so "Read AcpEditor.cpp" (kind read) and Bash
    // (kind execute) never count.
    static ToolCall classify(const AcpProtocol::AcpToolCall &tc, bool fromGoalJudge);

    // Empty = Off. Changing it never undoes a switch already made.
    void setTarget(const QString &model) { m_target = model; }
    const QString &target() const { return m_target; }

    void onTurnStarted() { m_inTurn = true; }
    // The model to switch to now, empty when nothing changes.
    QString onToolCall(const ToolCall &call, const Selection &current);
    // The selection to go back to, nullopt when the turn did not switch.
    std::optional<Selection> onTurnEnded();
    // The agent reported its model / effort. Once the session model is back,
    // returns the effort to set again if the switch changed it, else empty.
    QString onSelectionReported(const Selection &current);
    // The user's own model pick wins: nothing is restored.
    void onUserPickedModel();
    // The user's own effort pick while switched is the one to go back to.
    void onUserPickedEffort(const QString &effort);
    // New agent session (restart): no turn, nothing switched. Keeps the target.
    void reset();

    // Switched, or the restore has not landed yet.
    bool isSwitched() const { return m_phase != Phase::Idle; }
    // What the session really runs on while switched (persist this, not the
    // prewalk model); nullopt otherwise.
    std::optional<Selection> sessionSelection() const;

private:
    enum class Phase : std::uint8_t { Idle, Switched, Restoring };

    QString m_target;
    Selection m_session;
    Phase m_phase = Phase::Idle;
    bool m_inTurn = false;
};

#endif // ACP_PREWALK_H
