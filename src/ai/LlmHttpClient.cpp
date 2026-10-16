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

#include "LlmHttpClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <utility>

namespace ai {

LlmHttpClient::LlmHttpClient(QObject *parent)
    : ILlmHttpClient(parent),
      m_nam(new QNetworkAccessManager(this))
{
    m_idleTimer.setSingleShot(true);
    connect(&m_idleTimer, &QTimer::timeout, this, &LlmHttpClient::onIdleTimeout);
}

LlmHttpClient::~LlmHttpClient() { cancel(); }

QUrl LlmHttpClient::normalizeChatCompletionsUrl(const QUrl &base)
{
    QString s = base.toString().trimmed();
    if (s.isEmpty()) return base;
    while (s.endsWith(QLatin1Char('/'))) s.chop(1);
    if (s.endsWith(QLatin1String("/chat/completions"))) return QUrl(s);
    return QUrl(s + QLatin1String("/chat/completions"));
}

QUrl LlmHttpClient::normalizeUrl(const Request &req)
{
    if (req.apiFormat == ILlmHttpClient::ApiFormat::Anthropic) {
        QString s = req.url.toString().trimmed();
        if (s.isEmpty()) return req.url;
        while (s.endsWith(QLatin1Char('/'))) s.chop(1);
        if (s.endsWith(QLatin1String("/v1/messages"))) return QUrl(s);
        if (s.endsWith(QLatin1String("/v1"))) return QUrl(s + QLatin1String("/messages"));
        return QUrl(s + QLatin1String("/v1/messages"));
    }
    return normalizeChatCompletionsUrl(req.url);
}

QVector<QPair<QByteArray, QByteArray>> LlmHttpClient::authHeaders(const Request &req)
{
    QVector<QPair<QByteArray, QByteArray>> out;
    if (req.apiFormat == ILlmHttpClient::ApiFormat::Anthropic) {
        if (!req.apiKey.isEmpty())
            out.append({QByteArray("x-api-key"), req.apiKey.toUtf8()});
        out.append({QByteArray("anthropic-version"), QByteArray("2023-06-01")});
        return out;
    }
    if (!req.apiKey.isEmpty())
        out.append({QByteArray("Authorization"), ("Bearer " + req.apiKey).toUtf8()});
    return out;
}

QString LlmHttpClient::httpErrorText(int status, const QByteArray &body)
{
    QJsonParseError jerr{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &jerr);
    if (jerr.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonValue errV = doc.object().value(QLatin1String("error"));
        if (errV.isString() && !errV.toString().isEmpty())
            return errV.toString();
        if (errV.isObject()) {
            const QJsonValue msgV = errV.toObject().value(QLatin1String("message"));
            if (msgV.isString() && !msgV.toString().isEmpty())
                return msgV.toString();
        }
    }
    return QStringLiteral("HTTP %1").arg(status);
}

ILlmHttpClient::ApiFormat LlmHttpClient::apiFormatFromStored(int stored)
{
    return stored == static_cast<int>(ILlmHttpClient::ApiFormat::Anthropic)
        ? ILlmHttpClient::ApiFormat::Anthropic
        : ILlmHttpClient::ApiFormat::OpenAiCompatible;
}

QString LlmHttpClient::rejectReason(const Request &req)
{
    if (req.apiKey.trimmed().isEmpty())
        return QStringLiteral("API key is empty");
    return {};
}

QByteArray LlmHttpClient::buildPayload(const Request &req)
{
    if (req.apiFormat == ILlmHttpClient::ApiFormat::Anthropic) {
        QJsonArray messages;
        QJsonObject userMsg;
        userMsg.insert(QLatin1String("role"), QLatin1String("user"));
        if (req.images.isEmpty()) {
            userMsg.insert(QLatin1String("content"), req.prompt);
        } else {
            QJsonArray contentArr;
            QJsonObject textPart;
            textPart.insert(QLatin1String("type"), QLatin1String("text"));
            textPart.insert(QLatin1String("text"), req.prompt);
            contentArr.append(textPart);
            for (const auto &img : req.images) {
                QJsonObject source;
                source.insert(QLatin1String("type"), QLatin1String("base64"));
                source.insert(QLatin1String("media_type"), img.second);
                source.insert(QLatin1String("data"), QString::fromLatin1(img.first.toBase64()));
                QJsonObject imgPart;
                imgPart.insert(QLatin1String("type"), QLatin1String("image"));
                imgPart.insert(QLatin1String("source"), source);
                contentArr.append(imgPart);
            }
            userMsg.insert(QLatin1String("content"), contentArr);
        }
        messages.append(userMsg);

        QJsonObject body;
        body.insert(QLatin1String("model"), req.model);
        body.insert(QLatin1String("messages"), messages);
        body.insert(QLatin1String("stream"), true);
        body.insert(QLatin1String("max_tokens"), req.maxTokens > 0 ? req.maxTokens : 16000);
        if (!req.systemPrompt.isEmpty())
            body.insert(QLatin1String("system"), req.systemPrompt);
        if (!req.tools.isEmpty()) {
            QJsonArray tools;
            for (const Tool &t : req.tools) {
                QJsonObject o;
                o.insert(QLatin1String("name"), t.name);
                o.insert(QLatin1String("description"), t.description);
                o.insert(QLatin1String("input_schema"), t.inputSchema);
                tools.append(o);
            }
            body.insert(QLatin1String("tools"), tools);
            if (req.forceToolChoice) {
                QJsonObject choice;
                choice.insert(QLatin1String("type"), QLatin1String("tool"));
                choice.insert(QLatin1String("name"), req.tools.first().name);
                body.insert(QLatin1String("tool_choice"), choice);
            }
        }
        return QJsonDocument(body).toJson(QJsonDocument::Compact);
    }

    QJsonArray messages;

    if (!req.systemPrompt.isEmpty()) {
        QJsonObject sysMsg;
        sysMsg.insert(QLatin1String("role"),    QLatin1String("system"));
        sysMsg.insert(QLatin1String("content"), req.systemPrompt);
        messages.append(sysMsg);
    }

    QJsonObject userMsg;
    userMsg.insert(QLatin1String("role"),    QLatin1String("user"));
    if (req.images.isEmpty()) {
        userMsg.insert(QLatin1String("content"), req.prompt);
    } else {
        QJsonArray contentArr;
        QJsonObject textPart;
        textPart.insert(QLatin1String("type"), QLatin1String("text"));
        textPart.insert(QLatin1String("text"), req.prompt);
        contentArr.append(textPart);
        for (const auto &img : req.images) {
            QJsonObject urlObj;
            urlObj.insert(QLatin1String("url"),
                          QStringLiteral("data:%1;base64,%2")
                              .arg(img.second,
                                   QString::fromLatin1(img.first.toBase64())));
            urlObj.insert(QLatin1String("detail"), QLatin1String("low"));
            QJsonObject imgPart;
            imgPart.insert(QLatin1String("type"), QLatin1String("image_url"));
            imgPart.insert(QLatin1String("image_url"), urlObj);
            contentArr.append(imgPart);
        }
        userMsg.insert(QLatin1String("content"), contentArr);
    }
    messages.append(userMsg);

    QJsonObject body;
    body.insert(QLatin1String("model"),    req.model);
    body.insert(QLatin1String("messages"), messages);
    body.insert(QLatin1String("stream"),   true);
    if (req.maxTokens > 0)
        body.insert(QLatin1String("max_tokens"), req.maxTokens);
    if (!req.tools.isEmpty()) {
        QJsonArray tools;
        for (const Tool &t : req.tools) {
            QJsonObject fn;
            fn.insert(QLatin1String("name"), t.name);
            fn.insert(QLatin1String("description"), t.description);
            fn.insert(QLatin1String("parameters"), t.inputSchema);
            QJsonObject o;
            o.insert(QLatin1String("type"), QLatin1String("function"));
            o.insert(QLatin1String("function"), fn);
            tools.append(o);
        }
        body.insert(QLatin1String("tools"), tools);
        if (req.forceToolChoice) {
            QJsonObject fn;
            fn.insert(QLatin1String("name"), req.tools.first().name);
            QJsonObject choice;
            choice.insert(QLatin1String("type"), QLatin1String("function"));
            choice.insert(QLatin1String("function"), fn);
            body.insert(QLatin1String("tool_choice"), choice);
        }
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

void LlmHttpClient::openStream(const Request &req)
{
    if (m_reply) {
        // Caller error — but be defensive: cancel previous, then proceed.
        cancel();
    }
    const QString reject = rejectReason(req);
    if (!reject.isEmpty()) {
        emit errorOccurred(0, reject);
        return;
    }
    m_firstByteEmitted = false;
    m_anyTokenSeen = false;
    m_endedCleanly = false;
    m_anyToolSeen = false;
    m_toolCalls.clear();
    m_parser.reset();

    QNetworkRequest httpReq(normalizeUrl(req));
    httpReq.setHeader(QNetworkRequest::ContentTypeHeader, QLatin1String("application/json"));
    httpReq.setRawHeader("Accept", "text/event-stream");
    for (const auto &h : authHeaders(req))
        httpReq.setRawHeader(h.first, h.second);
    httpReq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    const QByteArray body = buildPayload(req);
    m_reply = m_nam->post(httpReq, body);

    connect(m_reply.data(), &QIODevice::readyRead,   this, &LlmHttpClient::onReadyRead);
    connect(m_reply.data(), &QNetworkReply::finished, this, &LlmHttpClient::onFinished);

    const int idleMs = qMax(5, req.idleTimeoutSec) * 1000;
    m_idleTimer.start(idleMs);
}

void LlmHttpClient::cancel()
{
    m_idleTimer.stop();
    if (m_reply) {
        QNetworkReply *r = m_reply.data();
        m_reply.clear();
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
}

void LlmHttpClient::onReadyRead()
{
    if (!m_reply) return;

    // Restart idle watchdog on every chunk.
    m_idleTimer.start();

    if (!m_firstByteEmitted) {
        // HTTP status check before emitting firstByteReceived.
        const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 400) {
            const QByteArray body = m_reply->readAll();
            emit errorOccurred(status, httpErrorText(status, body));
            cancel();
            return;
        }
        if (status > 0) {
            m_firstByteEmitted = true;
            emit firstByteReceived();
        }
    }

    const QByteArray bytes = m_reply->readAll();
    m_parser.feed(QByteArrayView(bytes),
                  [this](const SsePartialParser::Event &ev) {
        if (!m_reply) return;
        switch (ev.kind) {
        case SsePartialParser::EventKind::Token:
            m_anyTokenSeen = true;
            emit tokenReceived(ev.text);
            break;
        case SsePartialParser::EventKind::ToolDelta: {
            m_anyToolSeen = true;
            ToolCallAccum &c = m_toolCalls[ev.toolIndex];
            if (c.name.isEmpty()) c.name = ev.toolName;
            c.args += ev.text.toUtf8();
            break;
        }
        case SsePartialParser::EventKind::StopReason:
            emit stopReasonReceived(ev.text);
            break;
        case SsePartialParser::EventKind::Done:
            m_endedCleanly = true;
            flushToolCalls();
            emit streamEnded();
            cancel();
            break;
        case SsePartialParser::EventKind::Error:
            emit errorOccurred(0, ev.text);
            cancel();
            break;
        }
    });
}

void LlmHttpClient::onFinished()
{
    if (!m_reply) return;
    m_idleTimer.stop();

    const QNetworkReply::NetworkError err = m_reply->error();
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    QPointer<QNetworkReply> r = m_reply;
    m_reply.clear();
    if (r) r->deleteLater();

    if (m_endedCleanly) return;
    if (err == QNetworkReply::OperationCanceledError) return;
    if (err != QNetworkReply::NoError) {
        emit errorOccurred(status, QStringLiteral("Network error: ") + (r ? r->errorString() : QString()));
        return;
    }
    // Connection closed without [DONE]/finish_reason. If we got tokens, treat
    // as soft end; otherwise as error.
    if (m_anyTokenSeen || m_anyToolSeen) {
        flushToolCalls();
        emit streamEnded();
    } else emit errorOccurred(status, QStringLiteral("Stream closed before any token arrived"));
}

void LlmHttpClient::flushToolCalls()
{
    // Move out first: a slot may start a new stream, which clears the member.
    const QMap<int, ToolCallAccum> calls = std::move(m_toolCalls);
    m_toolCalls.clear();
    for (const ToolCallAccum &c : calls)
        emit toolCallReceived(c.name, c.args);
}

void LlmHttpClient::onIdleTimeout()
{
    if (!m_reply) return;
    const QString msg = (m_anyTokenSeen || m_anyToolSeen)
        ? QStringLiteral("No bytes from provider for the idle timeout window")
        : QStringLiteral("Provider did not respond before the idle timeout");
    emit errorOccurred(0, msg);
    cancel();
}

} // namespace ai
