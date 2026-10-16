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

#ifndef AI_COMMIT_MESSAGE_TOOL_H
#define AI_COMMIT_MESSAGE_TOOL_H

#include "ILlmHttpClient.h"

#include <cstdint>

#include <QByteArray>
#include <QStringList>
#include <QString>

namespace ai {

// The `submit_commit_message` function the commit-message model must call
// (docs/adr/0004). Pure: schema, fixed system prompt, argument validation and
// the join into `type(scope): subject\n\nbody`. No widgets, no network.
namespace CommitMessageTool {

inline constexpr const char *kToolName = "submit_commit_message";

// Conventional Commits types accepted for `type`.
const QStringList &allowedTypes();

ILlmHttpClient::Tool tool();

// Fixed system prompt sent with every request; not part of the editable
// template, so a customised template cannot remove the tool requirement.
QString systemPrompt();

enum class Problem : std::uint8_t {
    None,
    InvalidJson,
    InvalidType,
    InvalidScope,
    InvalidSubject,
    InvalidBody
};

struct Parsed {
    Problem problem = Problem::None;
    QString message;   // joined commit message when problem == None
    QString error;     // user-facing explanation when problem != None
    bool ok() const { return problem == Problem::None; }
};

// Validate the raw tool-call arguments. Only whitespace is repaired
// (trimmed); anything else invalid is a Problem and nothing is returned.
Parsed parseArguments(const QByteArray &argumentsJson);

// type(scope): subject, then a blank line and the body when present.
QString format(const QString &type, const QString &scope,
               const QString &subject, const QString &body);

} // namespace CommitMessageTool
} // namespace ai

#endif // AI_COMMIT_MESSAGE_TOOL_H
