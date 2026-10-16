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

#include "CommitMessageTool.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace ai {
namespace CommitMessageTool {

namespace {

bool hasLineBreak(const QString &s)
{
    return s.contains(QLatin1Char('\n')) || s.contains(QLatin1Char('\r'));
}

Parsed fail(Problem p, const QString &why)
{
    Parsed out;
    out.problem = p;
    out.error = why;
    return out;
}

} // namespace

const QStringList &allowedTypes()
{
    static const QStringList types = {QStringLiteral("feat"),  QStringLiteral("fix"),      QStringLiteral("docs"),
            QStringLiteral("style"), QStringLiteral("refactor"), QStringLiteral("perf"),
            QStringLiteral("test"),  QStringLiteral("build"),    QStringLiteral("ci"),
            QStringLiteral("chore"), QStringLiteral("revert")};
    return types;
}

ILlmHttpClient::Tool tool()
{
    QJsonArray types;
    for (const QString &t : allowedTypes()) types.append(t);

    auto str = [](const QString &description) {
        QJsonObject o;
        o.insert(QLatin1String("type"), QLatin1String("string"));
        o.insert(QLatin1String("description"), description);
        return o;
    };

    QJsonObject typeProp = str(QStringLiteral("Conventional Commits type."));
    typeProp.insert(QLatin1String("enum"), types);

    QJsonObject props;
    props.insert(QLatin1String("type"), typeProp);
    props.insert(QLatin1String("scope"),
                 str(QStringLiteral("Optional scope, e.g. a module or folder. No parentheses or colon. Omit when none fits.")));
    props.insert(QLatin1String("subject"),
                 str(QStringLiteral("One line, imperative mood, no trailing period. type(scope): subject must stay within 50 characters.")));
    props.insert(QLatin1String("body"),
                 str(QStringLiteral("Optional. Explains WHY, not WHAT. Wrap at 72 characters; bullet points for several concerns.")));

    QJsonObject schema;
    schema.insert(QLatin1String("type"), QLatin1String("object"));
    schema.insert(QLatin1String("properties"), props);
    schema.insert(QLatin1String("required"),
                  QJsonArray{QLatin1String("type"), QLatin1String("subject")});

    ILlmHttpClient::Tool t;
    t.name = QLatin1String(kToolName);
    t.description = QStringLiteral("Submit the finished git commit message. This is the only way to deliver it.");
    t.inputSchema = schema;
    return t;
}

QString systemPrompt()
{
    return QStringLiteral(
        "You write git commit messages. You MUST deliver the message by calling the "
        "`%1` tool in your very first response. Never reply with plain text, "
        "never put the message in text, and do not call any other tool.")
        .arg(QLatin1String(kToolName));
}

QString format(const QString &type, const QString &scope,
               const QString &subject, const QString &body)
{
    QString out = type;
    if (!scope.isEmpty()) out += QLatin1Char('(') + scope + QLatin1Char(')');
    out += QStringLiteral(": ") + subject;
    if (!body.isEmpty()) out += QStringLiteral("\n\n") + body;
    return out;
}

Parsed parseArguments(const QByteArray &argumentsJson)
{
    QJsonParseError jerr{};
    const QJsonDocument doc = QJsonDocument::fromJson(argumentsJson, &jerr);
    if (jerr.error != QJsonParseError::NoError || !doc.isObject())
        return fail(Problem::InvalidJson,
                    QCoreApplication::translate("ai::CommitMessageTool", "The tool arguments are not valid JSON (the reply may have been cut off)"));
    const QJsonObject o = doc.object();

    const QString type = o.value(QLatin1String("type")).toString().trimmed();
    if (!allowedTypes().contains(type))
        return fail(Problem::InvalidType,
                    QCoreApplication::translate("ai::CommitMessageTool", "Invalid commit type \"%1\"; expected one of: %2")
                        .arg(type, allowedTypes().join(QLatin1String(", "))));

    const QJsonValue scopeV = o.value(QLatin1String("scope"));
    if (!scopeV.isUndefined() && !scopeV.isNull() && !scopeV.isString())
        return fail(Problem::InvalidScope, QCoreApplication::translate("ai::CommitMessageTool", "The scope must be a string"));
    const QString scope = scopeV.toString().trimmed();
    if (hasLineBreak(scope) || scope.contains(QLatin1Char('(')) || scope.contains(QLatin1Char(')'))
        || scope.contains(QLatin1Char(':')))
        return fail(Problem::InvalidScope,
                    QCoreApplication::translate("ai::CommitMessageTool", "Invalid scope \"%1\": no parentheses, colon or line breaks allowed").arg(scope));

    const QString subject = o.value(QLatin1String("subject")).toString().trimmed();
    if (subject.isEmpty())
        return fail(Problem::InvalidSubject, QCoreApplication::translate("ai::CommitMessageTool", "The subject is empty"));
    if (hasLineBreak(subject))
        return fail(Problem::InvalidSubject, QCoreApplication::translate("ai::CommitMessageTool", "The subject must be a single line"));

    const QJsonValue bodyV = o.value(QLatin1String("body"));
    if (!bodyV.isUndefined() && !bodyV.isNull() && !bodyV.isString())
        return fail(Problem::InvalidBody, QCoreApplication::translate("ai::CommitMessageTool", "The body must be a string"));
    const QString body = bodyV.toString().trimmed();

    Parsed out;
    out.message = format(type, scope, subject, body);
    return out;
}

} // namespace CommitMessageTool
} // namespace ai
