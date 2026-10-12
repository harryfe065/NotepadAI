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

#ifndef ACP_AGENT_DEFINITION_H
#define ACP_AGENT_DEFINITION_H

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QStringList>

// Plain data describing one configured ACP agent (built-in or user-defined).
// Intentionally not a QObject so it can be copied and stored by value.
struct AcpAgentDefinition
{
    QString id;
    QString name;
    QString command;
    QStringList args;
    QMap<QString, QString> env;
    QString icon;
    bool builtin = false;
    // Interactive question forms: the `ask_user` MCP tool and ACP form
    // elicitation (AskUserQuestion). Off = the agent asks in plain text.
    bool interactiveQuestions = true;
};

// Shell-like splitter shared by the settings dialog and the registry loader.
// Whitespace separates tokens; single/double quotes preserve spaces; backslash
// escapes the next character. Used at save time *and* at load time so legacy
// data persisted under the old "one arg per line" rule is auto-migrated to
// real argv tokens.
inline QStringList parseShellLikeArgs(const QString &text)
{
    QStringList out;
    QString token;
    token.reserve(text.size());

    QChar quote;
    bool escaped = false;
    for (const QChar c : text) {
        if (escaped) {
            token.append(c);
            escaped = false;
            continue;
        }
        if (c == QLatin1Char('\\')) {
            escaped = true;
            continue;
        }
        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            } else {
                token.append(c);
            }
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            continue;
        }
        if (c.isSpace()) {
            if (!token.isEmpty()) {
                out.append(token);
                token.clear();
            }
            continue;
        }
        token.append(c);
    }

    if (escaped) {
        token.append(QLatin1Char('\\'));
    }
    if (!token.isEmpty()) {
        out.append(token);
    }
    return out;
}

inline QJsonObject acpAgentDefinitionToJson(const AcpAgentDefinition &def)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), def.id);
    obj.insert(QStringLiteral("name"), def.name);
    obj.insert(QStringLiteral("command"), def.command);

    QJsonArray argsArray;
    for (const QString &arg : def.args) {
        argsArray.append(arg);
    }
    obj.insert(QStringLiteral("args"), argsArray);
    // Marker so the loader knows args are already proper argv tokens and must
    // not be re-tokenized. Pre-marker entries may contain whitespace inside a
    // single string (legacy "one arg per line" save path) and need splitting.
    obj.insert(QStringLiteral("argsTokenized"), true);

    QJsonObject envObj;
    for (auto it = def.env.constBegin(); it != def.env.constEnd(); ++it) {
        envObj.insert(it.key(), it.value());
    }
    obj.insert(QStringLiteral("env"), envObj);

    obj.insert(QStringLiteral("icon"), def.icon);
    obj.insert(QStringLiteral("builtin"), def.builtin);
    obj.insert(QStringLiteral("interactiveQuestions"), def.interactiveQuestions);
    return obj;
}

inline AcpAgentDefinition acpAgentDefinitionFromJson(const QJsonObject &obj)
{
    AcpAgentDefinition def;
    def.id = obj.value(QStringLiteral("id")).toString();
    def.name = obj.value(QStringLiteral("name")).toString();
    def.command = obj.value(QStringLiteral("command")).toString();

    const QJsonArray argsArray = obj.value(QStringLiteral("args")).toArray();
    const bool tokenized =
        obj.value(QStringLiteral("argsTokenized")).toBool(false);
    def.args.reserve(argsArray.size());
    for (const auto &v : argsArray) {
        const QString s = v.toString();
        if (tokenized) {
            def.args.append(s);
        } else {
            const QStringList split = parseShellLikeArgs(s);
            for (const QString &t : split) {
                def.args.append(t);
            }
        }
    }

    const QJsonObject envObj = obj.value(QStringLiteral("env")).toObject();
    for (auto it = envObj.constBegin(); it != envObj.constEnd(); ++it) {
        def.env.insert(it.key(), it.value().toString());
    }

    def.icon = obj.value(QStringLiteral("icon")).toString();
    def.builtin = obj.value(QStringLiteral("builtin")).toBool(false);
    def.interactiveQuestions = obj.value(QStringLiteral("interactiveQuestions")).toBool(true);
    return def;
}

#endif // ACP_AGENT_DEFINITION_H
