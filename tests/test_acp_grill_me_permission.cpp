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


struct Fixture
{
    AcpConnection conn;
    RecordingChannel *channel = new RecordingChannel(&conn);

    Fixture()
    {
        conn.attachChannelForTest(channel);
        channel->start();
    }
};

} // namespace

// The connection's side of grill_me: no form elicitation, the tool's
// permission prompts auto-allowed.
class TestAcpGrillMePermission : public QObject
{
    Q_OBJECT

private slots:
    void neverAdvertisesElicitation();
    void elicitationRequestIsRejected();
    void reportsAttachedMcpServers();
    void grillMePermissionIsAutoAllowed();
};

void TestAcpGrillMePermission::neverAdvertisesElicitation()
{
    Fixture f;
    f.channel->emitStarted(); // sends initialize
    const QJsonObject init = f.channel->frames().value(0);
    QCOMPARE(init.value(QStringLiteral("method")).toString(), QStringLiteral("initialize"));
    const QJsonObject caps = init.value(QStringLiteral("params")).toObject()
                                 .value(QStringLiteral("clientCapabilities")).toObject();
    QVERIFY(!caps.isEmpty());
    QVERIFY(!caps.contains(QStringLiteral("elicitation")));
}

void TestAcpGrillMePermission::elicitationRequestIsRejected()
{
    Fixture f;
    f.channel->push(inboundRequest(7, "elicitation/create", QJsonObject{
        {QStringLiteral("sessionId"), QStringLiteral("s1")},
        {QStringLiteral("mode"), QStringLiteral("form")},
        {QStringLiteral("message"), QStringLiteral("Pick one")}}));
    QCOMPARE(f.channel->replyTo(7).value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toInt(), -32601);
}

void TestAcpGrillMePermission::reportsAttachedMcpServers()
{
    Fixture f;
    QVERIFY(!f.conn.hasMcpServer(QLatin1String("grill_me")));
    f.conn.setMcpServers(QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("grill_me")}}});
    QVERIFY(f.conn.hasMcpServer(QLatin1String("grill_me")));
    QVERIFY(!f.conn.hasMcpServer(QLatin1String("other")));
}

void TestAcpGrillMePermission::grillMePermissionIsAutoAllowed()
{
    Fixture f;
    f.conn.setAutoApprovePolicyProvider([]() { return QStringLiteral("manual"); });
    QSignalSpy prompted(&f.conn, &AcpConnection::permissionRequested);
    const QJsonArray options{
        QJsonObject{{QStringLiteral("optionId"), QStringLiteral("allow")}, {QStringLiteral("kind"), QStringLiteral("allow_once")}},
        QJsonObject{{QStringLiteral("optionId"), QStringLiteral("deny")}, {QStringLiteral("kind"), QStringLiteral("reject_once")}},
    };
    f.channel->push(inboundRequest(13, "session/request_permission", QJsonObject{
        {QStringLiteral("sessionId"), QStringLiteral("s1")},
        {QStringLiteral("toolCall"), QJsonObject{{QStringLiteral("toolCallId"), QStringLiteral("t1")},
                                                 {QStringLiteral("title"), QStringLiteral("mcp__grill_me__ask")}}},
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

QTEST_GUILESS_MAIN(TestAcpGrillMePermission)
#include "test_acp_grill_me_permission.moc"
