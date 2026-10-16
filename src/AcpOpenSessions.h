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

#ifndef ACP_OPEN_SESSIONS_H
#define ACP_OPEN_SESSIONS_H

#include <QByteArray>
#include <QString>
#include <QVector>

// The AI sessions open in each project dock, in dock order, so they come back
// as restored sessions on the next start (`Ai/OpenSessions`). Pure JSON.
namespace AcpOpenSessions {

inline constexpr char kSettingsKey[] = "Ai/OpenSessions";

struct Session
{
    QString id;        // NotepadAI session id (history file name)
    QString agentId;
    QString agentName; // shown if the agent was removed since
};

struct Group
{
    QString cwd;
    QString sshProfileId; // empty = local
    int current = 0;
    QVector<Session> sessions;
};

QByteArray toJson(const QVector<Group> &groups);
// Tolerant: unreadable input is empty; groups without a cwd or sessions and
// sessions without an id are dropped; current is clamped.
QVector<Group> fromJson(const QByteArray &json);

} // namespace AcpOpenSessions

#endif // ACP_OPEN_SESSIONS_H
