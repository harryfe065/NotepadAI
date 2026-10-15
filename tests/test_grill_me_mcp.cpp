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
#include <QProcess>
#include <QStandardPaths>

#include "GrillMeForm.h"
#include "GrillMeMcp.h"

namespace {

QJsonObject request(int id, const char *method, const QJsonObject &params = {})
{
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("method"), QString::fromLatin1(method)},
                       {QStringLiteral("params"), params}};
}

QJsonObject callParams(const char *argsJson)
{
    return QJsonObject{{QStringLiteral("name"), QStringLiteral("ask")},
                       {QStringLiteral("arguments"),
                        QJsonDocument::fromJson(QByteArray(argsJson)).object()}};
}

const char kValidArgs[] = R"({"questions":[{"id":"Q1","title":"Pick","type":"single",
    "options":["a","b"],"recommended":"a"}]})";
const char kInvalidArgs[] = R"({"questions":[{"id":"Q1","title":"Pick","type":"single"}]})";

QString resultText(const QJsonObject &reply)
{
    return reply.value(QStringLiteral("result")).toObject().value(QStringLiteral("content"))
        .toArray().at(0).toObject().value(QStringLiteral("text")).toString();
}

bool resultIsError(const QJsonObject &reply)
{
    return reply.value(QStringLiteral("result")).toObject().value(QStringLiteral("isError")).toBool();
}

} // namespace

class TestGrillMeMcp : public QObject
{
    Q_OBJECT

private slots:
    void listsGrillMeTool();
    void validCallReturnsAtOnce();
    void invalidCallNamesTheProblem();
    void notificationsGetNoReply();
    void serverEntryForLocalAndRemote();
    void nodeScriptServesTheSameProtocol();
};

void TestGrillMeMcp::listsGrillMeTool()
{
    const QJsonObject init = GrillMeMcp::handle(request(1, "initialize",
        {{QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18")}}));
    QCOMPARE(init.value(QStringLiteral("result")).toObject().value(QStringLiteral("protocolVersion")).toString(),
             QStringLiteral("2025-06-18"));
    QCOMPARE(init.value(QStringLiteral("result")).toObject().value(QStringLiteral("serverInfo")).toObject()
                 .value(QStringLiteral("name")).toString(),
             QStringLiteral("grill_me"));

    const QJsonObject list = GrillMeMcp::handle(request(2, "tools/list"));
    const QJsonArray tools = list.value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
    QCOMPARE(tools.size(), 1);
    const QJsonObject tool = tools.at(0).toObject();
    QCOMPARE(tool.value(QStringLiteral("name")).toString(), QStringLiteral("ask"));
    const QString description = tool.value(QStringLiteral("description")).toString();
    QVERIFY(!description.contains(QLatin1String("AskUserQuestion")));
    // Question rounds only: outside them the agent decides or asks in text.
    QVERIFY(description.contains(QLatin1String("Outside such rounds do not call it")));
    QVERIFY(description.contains(QLatin1String("grill")));
    // A skill's plain-text question format must map onto the tool (probed: without
    // this, Haiku answered /grill-me with "Q1" text every time).
    QVERIFY(description.contains(QLatin1String("describes the round's content, not its delivery")));
    QVERIFY(description.contains(QLatin1String("never call it twice in one turn")));
    QVERIFY(description.contains(QLatin1String("plan is settled")));
    QVERIFY(description.contains(QLatin1String("how or whether to proceed")));
    QVERIFY(description.contains(QLatin1String("[grill_me answers]")));
    QVERIFY(description.contains(QLatin1String("needs clarification")));
    QVERIFY(description.contains(QLatin1String("no answer")));
    QVERIFY(description.contains(QLatin1String("`recommended`")));
    QVERIFY(description.size() < 1700); // one dense paragraph, not a rulebook
    QVERIFY(description.contains(QLatin1String("END YOUR TURN")));
    const QJsonObject schema = tool.value(QStringLiteral("inputSchema")).toObject();
    QCOMPARE(schema.value(QStringLiteral("required")).toArray(), QJsonArray{QStringLiteral("questions")});
}

void TestGrillMeMcp::validCallReturnsAtOnce()
{
    const QJsonObject reply = GrillMeMcp::handle(request(3, "tools/call", callParams(kValidArgs)));
    QCOMPARE(reply.value(QStringLiteral("id")).toInt(), 3);
    QVERIFY(!resultIsError(reply));
    QVERIFY(resultText(reply).contains(QLatin1String("End your turn")));
}

void TestGrillMeMcp::invalidCallNamesTheProblem()
{
    const QJsonObject reply = GrillMeMcp::handle(request(4, "tools/call", callParams(kInvalidArgs)));
    QVERIFY(resultIsError(reply));
    QVERIFY2(resultText(reply).contains(QLatin1String("options")), qPrintable(resultText(reply)));

    QJsonObject wrongTool = callParams(kValidArgs);
    wrongTool.insert(QStringLiteral("name"), QStringLiteral("other"));
    QVERIFY(resultIsError(GrillMeMcp::handle(request(5, "tools/call", wrongTool))));
}

void TestGrillMeMcp::notificationsGetNoReply()
{
    QJsonObject note{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("method"), QStringLiteral("notifications/initialized")}};
    QVERIFY(GrillMeMcp::handle(note).isEmpty());
    const QJsonObject unknown = GrillMeMcp::handle(request(6, "resources/list"));
    QCOMPARE(unknown.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32601);
}

void TestGrillMeMcp::serverEntryForLocalAndRemote()
{
    const QJsonObject local = GrillMeMcp::serverEntry(false);
    QCOMPARE(local.value(QStringLiteral("name")).toString(), QStringLiteral("grill_me"));
    QCOMPARE(local.value(QStringLiteral("command")).toString(), QCoreApplication::applicationFilePath());
    QCOMPARE(local.value(QStringLiteral("args")).toArray(), QJsonArray{QStringLiteral("--grill-me-mcp")});
    QVERIFY(local.value(QStringLiteral("env")).isArray()); // array, or strict agents reject session/new

    const QJsonObject remote = GrillMeMcp::serverEntry(true);
    QCOMPARE(remote.value(QStringLiteral("command")).toString(), QStringLiteral("/bin/sh"));
    const QJsonArray args = remote.value(QStringLiteral("args")).toArray();
    QCOMPARE(args.size(), 3);
    QCOMPARE(args.at(0).toString(), QStringLiteral("-c"));
    QVERIFY(args.at(1).toString().contains(QLatin1String("command -v node")));
    QCOMPARE(args.at(2).toString(), GrillMeMcp::nodeScript());
}

void TestGrillMeMcp::nodeScriptServesTheSameProtocol()
{
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (node.isEmpty())
        QSKIP("node is not on PATH");
    QProcess proc;
    proc.start(node, {QStringLiteral("-e"), GrillMeMcp::nodeScript()});
    QVERIFY(proc.waitForStarted(10000));
    const QList<QJsonObject> requests{
        request(1, "initialize", {{QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18")}}),
        request(2, "tools/list"),
        request(3, "tools/call", callParams(kValidArgs)),
        request(4, "tools/call", callParams(kInvalidArgs)),
    };
    for (const QJsonObject &r : requests)
        proc.write(QJsonDocument(r).toJson(QJsonDocument::Compact) + '\n');
    proc.closeWriteChannel();
    QVERIFY(proc.waitForFinished(10000));
    const QList<QByteArray> lines = proc.readAllStandardOutput().trimmed().split('\n');
    QCOMPARE(lines.size(), 4);
    QHash<int, QJsonObject> replies;
    for (const QByteArray &line : lines) {
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        replies.insert(o.value(QStringLiteral("id")).toInt(), o);
    }
    QCOMPARE(replies.value(2).value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")),
             QJsonValue(QJsonArray{GrillMeForm::toolDefinition()}));
    QVERIFY(!resultIsError(replies.value(3)));
    QCOMPARE(resultText(replies.value(3)), QString::fromLatin1(GrillMeMcp::kShownGuide));
    QVERIFY(resultIsError(replies.value(4)));
}

QTEST_GUILESS_MAIN(TestGrillMeMcp)
#include "test_grill_me_mcp.moc"
