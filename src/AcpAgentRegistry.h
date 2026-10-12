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

#ifndef ACP_AGENT_REGISTRY_H
#define ACP_AGENT_REGISTRY_H

#include <QList>
#include <QObject>
#include <QString>

#include "AcpAgentDefinition.h"

class ApplicationSettings;

class AcpAgentRegistry : public QObject
{
    Q_OBJECT

public:
    explicit AcpAgentRegistry(ApplicationSettings *settings, QObject *parent = nullptr);

    static QString builtinClaudeCodeId();
    static QString builtinCodexId();
    static AcpAgentDefinition builtinClaudeCodeDefinition();
    static AcpAgentDefinition builtinCodexDefinition();

    QList<AcpAgentDefinition> agents() const;
    bool contains(const QString &id) const;
    AcpAgentDefinition agent(const QString &id) const;

    bool addAgent(const AcpAgentDefinition &def);
    bool updateAgent(const AcpAgentDefinition &def);
    bool removeAgent(const QString &id);

    // Moves `id` to `toIndex` (index in the list after the move). False, and
    // no `changed()`, when the id is unknown, the index is out of range, or
    // the agent is already there.
    bool moveAgent(const QString &id, int toIndex);

    // Inserts a user-defined copy right after `id` with a fresh id and a
    // unique "<name> (copy N)" name. Returns the new id, or empty if `id` is
    // unknown.
    QString cloneAgent(const QString &id);

    QString defaultAgentId() const;
    void setDefaultAgentId(const QString &id);

    QString autoApprovePolicy() const;
    void setAutoApprovePolicy(const QString &policy);

    // Per-agent UI preference (model / mode / effort etc.). Stored in
    // ApplicationSettings as a single JSON object so the user's last
    // selection survives session restart and new-session creation.
    // `key` is the preference name (e.g. "model", "mode", or the effort
    // config-option id). Empty string means "no saved value".
    QString agentPreference(const QString &agentId, const QString &key) const;
    void setAgentPreference(const QString &agentId, const QString &key, const QString &value);

signals:
    void changed();
    void defaultAgentIdChanged(const QString &id);
    void autoApprovePolicyChanged(const QString &policy);

private:
    void load();
    int indexOf(const QString &id) const;
    QString uniqueCopyName(const QString &name) const;
    void persistUserAgents();

    ApplicationSettings *m_settings;
    QList<AcpAgentDefinition> m_agents;
};

#endif // ACP_AGENT_REGISTRY_H
