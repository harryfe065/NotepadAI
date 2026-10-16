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

#include "CommitMessageGenerator.h"

#include "ApplicationSettings.h"
#include "CommitMessageTool.h"
#include "CredentialStore.h"
#include "DiffCompressor.h"
#include "LlmHttpClient.h"
#include "PromptAssembler.h"
#include "RulesLocator.h"

#include "git/CommitComposer.h"

#include <QPlainTextEdit>
#include <QSet>
#include <QTextCursor>
#include <QTimer>

namespace ai {

namespace {

// url|model pairs whose server rejected a named tool_choice this session
// (docs/adr/0004). In memory only; UI thread only.
QSet<QString> &noForcedToolChoice()
{
    static QSet<QString> set;
    return set;
}

QString forcedChoiceKey(const ILlmHttpClient::Request &req)
{
    return req.url.toString() + QLatin1Char('|') + req.model;
}

QString clipped(const QString &text, int max)
{
    const QString one = text.simplified();
    return one.size() <= max ? one : one.left(max) + QChar(0x2026);
}

} // namespace

CommitMessageGenerator::CommitMessageGenerator(ApplicationSettings *settings,
                                               CredentialStore *credStore,
                                               QObject *parent)
    : QObject(parent),
      m_settings(settings),
      m_credStore(credStore),
      m_http(new LlmHttpClient(this))
{
    wireClient();
}

CommitMessageGenerator::~CommitMessageGenerator() = default;

void CommitMessageGenerator::wireClient()
{
    connect(m_http, &ILlmHttpClient::firstByteReceived,  this, &CommitMessageGenerator::onFirstByte);
    connect(m_http, &ILlmHttpClient::tokenReceived,      this, &CommitMessageGenerator::onToken);
    connect(m_http, &ILlmHttpClient::toolCallReceived,   this, &CommitMessageGenerator::onToolCall);
    connect(m_http, &ILlmHttpClient::stopReasonReceived, this, &CommitMessageGenerator::onStopReason);
    connect(m_http, &ILlmHttpClient::streamEnded,        this, &CommitMessageGenerator::onStreamEnded);
    connect(m_http, &ILlmHttpClient::errorOccurred,      this, &CommitMessageGenerator::onStreamError);
}

void CommitMessageGenerator::setHttpClientForTesting(ILlmHttpClient *client)
{
    Q_ASSERT(client);
    if (m_http) {
        m_http->disconnect(this);
        m_http->deleteLater();
    }
    m_http = client;
    m_http->setParent(this);
    wireClient();
}

bool CommitMessageGenerator::canFireGenerate(const QString &workspaceRoot,
                                             CommitComposer *composer,
                                             QString *whyNot) const
{
    auto fail = [whyNot](const QString &msg) {
        if (whyNot) *whyNot = msg;
        return false;
    };
    if (!composer)        return fail(QObject::tr("No commit message editor"));
    if (workspaceRoot.isEmpty()) return fail(QObject::tr("No workspace selected"));
    if (!m_settings)      return fail(QObject::tr("Settings unavailable"));

    if (m_state != State::Idle && m_state != State::Error) {
        return fail(QObject::tr("A generation is already in progress"));
    }
    if (m_settings->commitMessageProviderUrl().trimmed().isEmpty()) {
        return fail(QObject::tr("Configure the AI provider URL in Preferences → AI"));
    }
    if (m_settings->commitMessageModel().trimmed().isEmpty()) {
        return fail(QObject::tr("Configure the AI model in Preferences → AI"));
    }
    if (m_credStore && !m_credStore->isApiKeyAvailable()) {
        return fail(QObject::tr("Configure the AI API key in Preferences → AI"));
    }
    return true;
}

void CommitMessageGenerator::setState(State s)
{
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

void CommitMessageGenerator::trigger(const QString &workspaceRoot,
                                     const QString &submoduleRoot,
                                     CommitComposer *composer,
                                     const QString &subjectHint,
                                     const QByteArray &compressedDiff)
{
    QString why;
    if (!canFireGenerate(workspaceRoot, composer, &why)) {
        emit errorOccurred(why);
        return;
    }
    Q_ASSERT(m_settings);

    QString apiKey;
    if (m_credStore) {
        QString err;
        apiKey = m_credStore->retrieveApiKey(&err);
        if (apiKey.isEmpty()) {
            emit errorOccurred(err.isEmpty() ? tr("AI API key not available") : err);
            return;
        }
    }

    const QString rules = RulesLocator::locate(submoduleRoot, workspaceRoot,
                                               m_settings->commitMessageRulesByteBudget());
    const QByteArray diff = DiffCompressor::compress(
        compressedDiff, m_settings->commitMessageDiffByteBudget());
    const QString prompt = PromptAssembler::assemble(
        m_settings->commitMessagePromptTemplate(), rules, subjectHint, diff);

    m_target = composer;
    m_currentRepoKey = workspaceRoot;
    m_retryPending = false;
    resetReply();

    ILlmHttpClient::Request req;
    req.url = QUrl(m_settings->commitMessageProviderUrl());
    req.model = m_settings->commitMessageModel();
    req.apiKey = apiKey;
    req.systemPrompt = CommitMessageTool::systemPrompt();
    req.prompt = prompt;
    req.idleTimeoutSec = m_settings->commitMessageStreamIdleTimeoutSec();
    req.apiFormat = LlmHttpClient::apiFormatFromStored(m_settings->commitMessageApiFormat());
    req.tools.append(CommitMessageTool::tool());
    req.forceToolChoice = !noForcedToolChoice().contains(forcedChoiceKey(req));
    m_request = req;

    setState(State::Authenticating);
    m_http->openStream(req);
}

void CommitMessageGenerator::cancel()
{
    if (m_state == State::Idle || m_state == State::Cancelling) return;
    setState(State::Cancelling);
    m_retryPending = false;
    m_http->cancel();
    setState(State::Idle);
    m_target.clear();
    m_currentRepoKey.clear();
}

void CommitMessageGenerator::cancelIfTarget(CommitComposer *composer)
{
    if (m_state == State::Idle) return;
    if (m_target.data() == composer) cancel();
}

void CommitMessageGenerator::resetReply()
{
    m_textReply.clear();
    m_stopReason.clear();
    m_toolArgs.clear();
    m_haveCall = false;
}

void CommitMessageGenerator::onFirstByte()
{
    if (m_state == State::Cancelling || m_state == State::Idle) return;
    setState(State::Streaming);
}

void CommitMessageGenerator::onToken(const QString &chunk)
{
    // Text is never the message; keep a little of it to explain the error.
    if (m_textReply.size() < 400) m_textReply.append(chunk);
}

void CommitMessageGenerator::onToolCall(const QString &name, const QByteArray &argumentsJson)
{
    // First submit_commit_message wins; other calls are ignored.
    if (m_haveCall || name != QLatin1String(CommitMessageTool::kToolName)) return;
    m_haveCall = true;
    m_toolArgs = argumentsJson;
}

void CommitMessageGenerator::onStopReason(const QString &reason)
{
    m_stopReason = reason;
}

void CommitMessageGenerator::onStreamEnded()
{
    if (m_state != State::Authenticating && m_state != State::Streaming) return;

    const QString tool = QLatin1String(CommitMessageTool::kToolName);
    QString error;
    QString message;

    if (m_stopReason == QLatin1String("max_tokens") || m_stopReason == QLatin1String("length")) {
        error = tr("The response was cut off (token limit reached) before the commit message was complete");
    } else if (!m_haveCall) {
        const QString text = clipped(m_textReply, 120);
        error = text.isEmpty()
            ? tr("The model did not call %1").arg(tool)
            : tr("The model replied with text instead of calling %1: \u201c%2\u201d").arg(tool, text);
    } else {
        const CommitMessageTool::Parsed parsed = CommitMessageTool::parseArguments(m_toolArgs);
        if (parsed.ok()) message = parsed.message;
        else error = parsed.error;
    }

    if (!error.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(error);
        teardownAfterError();
        return;
    }

    insertMessage(message);
    setState(State::Idle);
    m_target.clear();
    m_currentRepoKey.clear();
}

void CommitMessageGenerator::insertMessage(const QString &message)
{
    if (!m_target) return;
    QPlainTextEdit *edit = m_target->edit();
    if (!edit) return;
    // One edit block: Ctrl+Z restores whatever the user had typed.
    QTextCursor cursor(edit->document());
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.insertText(message);
    cursor.endEditBlock();
}

void CommitMessageGenerator::onStreamError(int httpStatus, const QString &message)
{
    if (m_state != State::Authenticating && m_state != State::Streaming) return;

    // Some OpenAI-compatible servers reject a named tool_choice. Resend once
    // without it (the system prompt still demands the call) and remember the
    // endpoint for the session so later clicks skip the failing request.
    if (m_request.forceToolChoice && (httpStatus == 400 || httpStatus == 422)
        && message.contains(QLatin1String("tool_choice"), Qt::CaseInsensitive)) {
        noForcedToolChoice().insert(forcedChoiceKey(m_request));
        m_request.forceToolChoice = false;
        m_retryPending = true;
        resetReply();
        // The client may still be unwinding the failed reply: reopen on the
        // next loop turn, never from inside its errorOccurred emission.
        QTimer::singleShot(0, this, [this]() {
            if (!m_retryPending) return;
            m_retryPending = false;
            m_http->openStream(m_request);
        });
        return;
    }

    setState(State::Error);
    emit errorOccurred(message);
    teardownAfterError();
}

void CommitMessageGenerator::teardownAfterError()
{
    setState(State::Idle);
    m_target.clear();
    m_currentRepoKey.clear();
}

} // namespace ai
