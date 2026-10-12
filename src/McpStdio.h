/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef MCP_STDIO_H
#define MCP_STDIO_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

// Minimal stdio MCP server shared by the app's built-in servers. They run
// before QApplication exists, so this needs QtCore only.
namespace McpStdio {

struct Server
{
    const char *name;
    QJsonArray (*tools)();
    // tools/call params -> CallToolResult.
    QJsonObject (*call)(const QJsonObject &params);
};

// One request or notification. Empty object = no response (notification).
QJsonObject dispatch(const Server &server, const QJsonObject &msg);
// Encode the reply as newline JSON or Content-Length. Empty = no write.
QByteArray encodeReply(const Server &server, const QByteArray &jsonRequest, bool newlineFraming);
// Serve stdin/stdout until EOF. Claude Code 2.1+ uses newline JSON; older
// clients use Content-Length framing.
int run(const Server &server);

QJsonObject textResult(const QString &text, bool isError);

} // namespace McpStdio

#endif // MCP_STDIO_H
