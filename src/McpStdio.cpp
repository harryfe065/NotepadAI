/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "McpStdio.h"

#include <QJsonDocument>
#include <QString>
#include <QStringList>

#include <cstdio>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace McpStdio {

namespace {

QJsonObject rpcResult(const QJsonValue &id, const QJsonObject &result)
{
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("result"), result},
    };
}

QJsonObject rpcError(const QJsonValue &id, int code, const QString &message)
{
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("error"), QJsonObject{
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message},
        }},
    };
}

bool readExact(char *buf, int n)
{
    int got = 0;
    while (got < n) {
        const int r = static_cast<int>(std::fread(buf + got, 1, static_cast<size_t>(n - got), stdin));
        if (r <= 0)
            return false;
        got += r;
    }
    return true;
}

QByteArray readFramed()
{
    QByteArray headers;
    char ch;
    int crlfStreak = 0;
    while (crlfStreak < 2) {
        if (std::fread(&ch, 1, 1, stdin) != 1)
            return {};
        headers.append(ch);
        if (ch == '\r')
            continue;
        if (ch == '\n')
            ++crlfStreak;
        else
            crlfStreak = 0;
        if (headers.size() > 64 * 1024)
            return {};
    }
    int length = -1;
    const QString hs = QString::fromUtf8(headers);
    const QStringList lines = hs.split(QStringLiteral("\r\n"));
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("Content-Length:"), Qt::CaseInsensitive)) {
            length = line.mid(15).trimmed().toInt();
            break;
        }
    }
    if (length < 0 || length > 4 * 1024 * 1024)
        return {};
    QByteArray body(length, Qt::Uninitialized);
    if (!readExact(body.data(), length))
        return {};
    return body;
}

QByteArray readLine()
{
    QByteArray line;
    char ch;
    while (std::fread(&ch, 1, 1, stdin) == 1) {
        if (ch == '\n')
            break;
        if (ch == '\r')
            continue;
        line.append(ch);
        if (line.size() > 4 * 1024 * 1024)
            return {};
    }
    return line;
}

void writeBytes(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fflush(stdout);
}

} // namespace

QJsonObject textResult(const QString &text, bool isError)
{
    return QJsonObject{
        {QStringLiteral("content"), QJsonArray{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), text},
        }}},
        {QStringLiteral("isError"), isError},
    };
}

QJsonObject dispatch(const Server &server, const QJsonObject &msg)
{
    const QString method = msg.value(QStringLiteral("method")).toString();
    const QJsonValue id = msg.value(QStringLiteral("id"));
    const QString name = QString::fromLatin1(server.name);
    if (method == QLatin1String("server/discover")) {
        return rpcResult(id, QJsonObject{
            {QStringLiteral("resultType"), QStringLiteral("complete")},
            {QStringLiteral("supportedVersions"),
             QJsonArray{QStringLiteral("2025-11-25")}},
            {QStringLiteral("capabilities"), QJsonObject{
                {QStringLiteral("tools"), QJsonObject{}},
            }},
            {QStringLiteral("_meta"), QJsonObject{
                {QStringLiteral("io.modelcontextprotocol/serverInfo"), QJsonObject{
                    {QStringLiteral("name"), name},
                    {QStringLiteral("version"), QStringLiteral("1")},
                }},
            }},
        });
    }
    if (method == QLatin1String("initialize")) {
        QString ver = msg.value(QStringLiteral("params")).toObject()
                          .value(QStringLiteral("protocolVersion")).toString();
        if (ver.isEmpty())
            ver = QStringLiteral("2025-11-25");
        return rpcResult(id, QJsonObject{
            {QStringLiteral("protocolVersion"), ver},
            {QStringLiteral("capabilities"), QJsonObject{
                {QStringLiteral("tools"), QJsonObject{}},
            }},
            {QStringLiteral("serverInfo"), QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("version"), QStringLiteral("1")},
            }},
        });
    }
    if (method.startsWith(QLatin1String("notifications/")) || method == QLatin1String("initialized"))
        return {};
    if (method == QLatin1String("tools/list"))
        return rpcResult(id, QJsonObject{{QStringLiteral("tools"), server.tools()}});
    if (method == QLatin1String("tools/listChanged"))
        return {};
    if (method == QLatin1String("tools/call"))
        return rpcResult(id, server.call(msg.value(QStringLiteral("params")).toObject()));
    if (method == QLatin1String("ping"))
        return rpcResult(id, QJsonObject{});
    if (!id.isUndefined() && !id.isNull())
        return rpcError(id, -32601, QStringLiteral("Method not found"));
    return {};
}

QByteArray encodeReply(const Server &server, const QByteArray &jsonRequest, bool newlineFraming)
{
    const QJsonDocument doc = QJsonDocument::fromJson(jsonRequest);
    if (!doc.isObject())
        return {};
    const QJsonObject reply = dispatch(server, doc.object());
    if (reply.isEmpty())
        return {};
    const QByteArray json = QJsonDocument(reply).toJson(QJsonDocument::Compact);
    if (newlineFraming)
        return json + '\n';
    return QByteArrayLiteral("Content-Length: ")
        + QByteArray::number(json.size())
        + QByteArrayLiteral("\r\n\r\n")
        + json;
}

int run(const Server &server)
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    for (;;) {
        const int c = std::fgetc(stdin);
        if (c == EOF)
            return 0;
        std::ungetc(c, stdin);
        const bool newline = (c == '{');
        const QByteArray body = newline ? readLine() : readFramed();
        if (body.isEmpty())
            return 0;
        writeBytes(encodeReply(server, body, newline));
    }
}

} // namespace McpStdio
