/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef GRILL_ME_MCP_H
#define GRILL_ME_MCP_H

#include <QJsonObject>
#include <QString>

// The `grill_me` MCP server with one tool, `ask`. It only validates and
// returns: the chat builds the form from the tool call's rawInput and sends
// the answers as the next user prompt, so the call never blocks.
class GrillMeMcp
{
public:
    static constexpr const char *kCliFlag = "--grill-me-mcp";
    static constexpr const char *kShownGuide =
        "Form shown to the user. End your turn now: do not restate the questions as text, do "
        "not call another tool and do not continue the work. The answers arrive as the next "
        "user message, beginning with [grill_me answers].";

    static int run();
    // One request or notification. Empty object = no response.
    static QJsonObject handle(const QJsonObject &msg);
    // ACP `session/new` mcpServers entry. Local: this executable. Remote: the
    // Node script through /bin/sh, which exits when node is missing.
    static QJsonObject serverEntry(bool remote);
    // Self-contained Node.js server speaking newline-delimited JSON-RPC.
    static QString nodeScript();
};

#endif // GRILL_ME_MCP_H
