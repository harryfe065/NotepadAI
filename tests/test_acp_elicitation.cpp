/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "AcpConnection.h"
#include "IAcpProcessChannel.h"

namespace {

// Records outbound frames; inbound frames are pushed as stdout chunks.
class RecordingChannel : public IAcpProcessChannel
{
public:
    explicit RecordingChannel(QObject *parent = nullptr) : IAcpProcessChannel(parent) {}

    void start() override { m_running = true; }
    void write(const QByteArray &bytes) override { written.append(bytes); }
    void kill() override { m_running = false; }
    bool isRunning() const override { return m_running; }

    void push(const QJsonObject &frame)
    {
        emit readyReadStdout(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');
    }
    void emitStarted() { emit started(); }

    // Outbound frames, parsed.
    QList<QJsonObject> frames() const
    {
        QList<QJsonObject> out;
        for (const QByteArray &line : written.split('\n')) {
            if (!line.trimmed().isEmpty())
                out.append(QJsonDocument::fromJson(line).object());
        }
        return out;
    }
    QJsonObject replyTo(int id) const
    {
        for (const QJsonObject &f : frames()) {
            if (!f.contains(QStringLiteral("method")) && f.value(QStringLiteral("id")).toInt() == id)
                return f;
        }
        return {};
    }

    QByteArray written;

private:
    bool m_running = false;
};

QJsonObject inboundRequest(int id, const char *method, const QJsonObject &params)
{
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("method"), QString::fromLatin1(method)},
                       {QStringLiteral("params"), params}};
}

QJsonObject formRequest()
{
    return QJsonObject{
        {QStringLiteral("sessionId"), QStringLiteral("s1")},
        {QStringLiteral("toolCallId"), QStringLiteral("toolu_1")},
        {QStringLiteral("mode"), QStringLiteral("form")},
        {QStringLiteral("message"), QStringLiteral("Pick one")},
        {QStringLiteral("requestedSchema"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("properties"), QJsonObject{
                {QStringLiteral("question_0"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}}},
    };
}

struct Fixture
{
    AcpConnection conn;
    RecordingChannel *channel = new RecordingChannel(&conn);

    explicit Fixture(bool elicitation)
    {
        conn.setElicitationEnabled(elicitation);
        conn.attachChannelForTest(channel);
        channel->start();
    }
};

} // namespace

class TestAcpElicitation : public QObject
{
    Q_OBJECT

private slots:
    void advertisesFormOnlyWhenEnabled();
    void formRequestIsSurfacedAndAccepted();
    void declineCarriesNoContent();
    void unsupportedModeIsRejected();
    void cancelRequestClosesThePendingForm();
    void processExitCancelsPendingForms();
    void askUserPermissionIsAutoAllowed();
};

void TestAcpElicitation::advertisesFormOnlyWhenEnabled()
{
    for (bool enabled : {true, false}) {
        Fixture f(enabled);
        f.channel->emitStarted(); // sends initialize
        const QJsonObject init = f.channel->frames().value(0);
        QCOMPARE(init.value(QStringLiteral("method")).toString(), QStringLiteral("initialize"));
        const QJsonObject caps = init.value(QStringLiteral("params")).toObject()
                                     .value(QStringLiteral("clientCapabilities")).toObject();
        QCOMPARE(caps.contains(QStringLiteral("elicitation")), enabled);
        if (enabled) {
            QVERIFY(caps.value(QStringLiteral("elicitation")).toObject()
                        .value(QStringLiteral("form")).isObject());
        }
    }
}

void TestAcpElicitation::formRequestIsSurfacedAndAccepted()
{
    Fixture f(true);
    QSignalSpy spy(&f.conn, &AcpConnection::elicitationRequested);
    f.channel->push(inboundRequest(7, "elicitation/create", formRequest()));
    QCOMPARE(spy.size(), 1);
    const auto req = spy.at(0).at(0).value<AcpProtocol::AcpElicitationRequest>();
    QCOMPARE(req.requestId, QStringLiteral("7"));
    QCOMPARE(req.toolCallId, QStringLiteral("toolu_1"));
    QCOMPARE(req.message, QStringLiteral("Pick one"));
    QVERIFY(req.requestedSchema.contains(QStringLiteral("properties")));
    QVERIFY(f.channel->replyTo(7).isEmpty()); // waits for the user

    f.conn.respondToElicitation(req.requestId, QStringLiteral("accept"),
                                {{QStringLiteral("question_0"), QStringLiteral("a")}});
    const QJsonObject result = f.channel->replyTo(7).value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("action")).toString(), QStringLiteral("accept"));
    QCOMPARE(result.value(QStringLiteral("content")).toObject()
                 .value(QStringLiteral("question_0")).toString(), QStringLiteral("a"));

    // A second answer for the same request is ignored.
    const qsizetype before = f.channel->written.size();
    f.conn.respondToElicitation(req.requestId, QStringLiteral("decline"));
    QCOMPARE(f.channel->written.size(), before);
}

void TestAcpElicitation::declineCarriesNoContent()
{
    Fixture f(true);
    f.channel->push(inboundRequest(8, "elicitation/create", formRequest()));
    f.conn.respondToElicitation(QStringLiteral("8"), QStringLiteral("decline"));
    const QJsonObject result = f.channel->replyTo(8).value(QStringLiteral("result")).toObject();
    QCOMPARE(result, QJsonObject({{QStringLiteral("action"), QStringLiteral("decline")}}));
}

void TestAcpElicitation::unsupportedModeIsRejected()
{
    Fixture f(true);
    QJsonObject params = formRequest();
    params.insert(QStringLiteral("mode"), QStringLiteral("url"));
    f.channel->push(inboundRequest(9, "elicitation/create", params));
    QCOMPARE(f.channel->replyTo(9).value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toInt(), -32602);

    // Form is the default when `mode` is omitted.
    QSignalSpy spy(&f.conn, &AcpConnection::elicitationRequested);
    QJsonObject noMode = formRequest();
    noMode.remove(QStringLiteral("mode"));
    f.channel->push(inboundRequest(15, "elicitation/create", noMode));
    QCOMPARE(spy.size(), 1);

    Fixture off(false); // never advertised: even form mode is rejected
    off.channel->push(inboundRequest(10, "elicitation/create", formRequest()));
    QCOMPARE(off.channel->replyTo(10).value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toInt(), -32602);
}

void TestAcpElicitation::cancelRequestClosesThePendingForm()
{
    Fixture f(true);
    QSignalSpy closed(&f.conn, &AcpConnection::elicitationClosed);
    f.channel->push(inboundRequest(11, "elicitation/create", formRequest()));
    f.channel->push(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                {QStringLiteral("method"), QStringLiteral("$/cancel_request")},
                                {QStringLiteral("params"), QJsonObject{{QStringLiteral("requestId"), 11}}}});
    QCOMPARE(f.channel->replyTo(11).value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toInt(), -32800);
    QCOMPARE(closed.size(), 1);
    QCOMPARE(closed.at(0).at(0).toString(), QStringLiteral("11"));
}

void TestAcpElicitation::processExitCancelsPendingForms()
{
    Fixture f(true);
    QSignalSpy closed(&f.conn, &AcpConnection::elicitationClosed);
    f.channel->push(inboundRequest(12, "elicitation/create", formRequest()));
    f.conn.simulateProcessFinished(0, QProcess::NormalExit);
    QCOMPARE(closed.size(), 1);
}

void TestAcpElicitation::askUserPermissionIsAutoAllowed()
{
    Fixture f(false);
    f.conn.setAutoApprovePolicyProvider([]() { return QStringLiteral("manual"); });
    QSignalSpy prompted(&f.conn, &AcpConnection::permissionRequested);
    const QJsonArray options{
        QJsonObject{{QStringLiteral("optionId"), QStringLiteral("allow")}, {QStringLiteral("kind"), QStringLiteral("allow_once")}},
        QJsonObject{{QStringLiteral("optionId"), QStringLiteral("deny")}, {QStringLiteral("kind"), QStringLiteral("reject_once")}},
    };
    f.channel->push(inboundRequest(13, "session/request_permission", QJsonObject{
        {QStringLiteral("sessionId"), QStringLiteral("s1")},
        {QStringLiteral("toolCall"), QJsonObject{{QStringLiteral("toolCallId"), QStringLiteral("t1")},
                                                 {QStringLiteral("title"), QStringLiteral("mcp__notepadai__ask_user")}}},
        {QStringLiteral("options"), options}}));
    QCOMPARE(prompted.size(), 0);
    QVERIFY(!f.channel->replyTo(13).isEmpty());
    QVERIFY(f.channel->written.contains("allow"));

    // Any other tool still prompts under the manual policy.
    f.channel->push(inboundRequest(14, "session/request_permission", QJsonObject{
        {QStringLiteral("sessionId"), QStringLiteral("s1")},
        {QStringLiteral("toolCall"), QJsonObject{{QStringLiteral("toolCallId"), QStringLiteral("t2")},
                                                 {QStringLiteral("title"), QStringLiteral("Bash")}}},
        {QStringLiteral("options"), options}}));
    QCOMPARE(prompted.size(), 1);
}

QTEST_GUILESS_MAIN(TestAcpElicitation)
#include "test_acp_elicitation.moc"
