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

#include "AcpOpenSessions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace AcpOpenSessions {

QByteArray toJson(const QVector<Group> &groups)
{
    QJsonArray arr;
    for (const Group &g : groups) {
        QJsonArray sessions;
        for (const Session &s : g.sessions) {
            QJsonObject so;
            so.insert(QStringLiteral("id"), s.id);
            so.insert(QStringLiteral("agentId"), s.agentId);
            so.insert(QStringLiteral("agentName"), s.agentName);
            sessions.append(so);
        }
        QJsonObject go;
        go.insert(QStringLiteral("cwd"), g.cwd);
        if (!g.sshProfileId.isEmpty())
            go.insert(QStringLiteral("ssh"), g.sshProfileId);
        go.insert(QStringLiteral("current"), g.current);
        go.insert(QStringLiteral("sessions"), sessions);
        arr.append(go);
    }
    return QJsonDocument(arr).toJson(QJsonDocument::Compact);
}

QVector<Group> fromJson(const QByteArray &json)
{
    QVector<Group> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray())
        return out;
    const QJsonArray arr = doc.array();
    out.reserve(arr.size());
    for (const auto gv : arr) {
        const QJsonObject go = gv.toObject();
        Group g;
        g.cwd = go.value(QStringLiteral("cwd")).toString();
        g.sshProfileId = go.value(QStringLiteral("ssh")).toString();
        if (g.cwd.isEmpty())
            continue;
        const QJsonArray sessions = go.value(QStringLiteral("sessions")).toArray();
        g.sessions.reserve(sessions.size());
        for (const auto sv : sessions) {
            const QJsonObject so = sv.toObject();
            Session s;
            s.id = so.value(QStringLiteral("id")).toString();
            if (s.id.isEmpty())
                continue;
            s.agentId = so.value(QStringLiteral("agentId")).toString();
            s.agentName = so.value(QStringLiteral("agentName")).toString();
            g.sessions.append(std::move(s));
        }
        if (g.sessions.isEmpty())
            continue;
        g.current = qBound(0, go.value(QStringLiteral("current")).toInt(), int(g.sessions.size()) - 1);
        out.append(std::move(g));
    }
    return out;
}

} // namespace AcpOpenSessions
