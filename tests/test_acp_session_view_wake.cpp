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
#include <QLabel>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <memory>

#include "AcpConnection.h"
#include "AcpSessionModel.h"
#include "IAcpProcessChannel.h"
#include "AcpMessageWidget.h"
#include "AcpSessionView.h"
#include "AcpToolCallCard.h"
#include "AcpTurnFoldRow.h"

// AcpSessionView fed through AcpConnection + AcpSessionModel with frames shaped
// like claude-agent-acp 0.90.0's real wire.

namespace {

class RecordingChannel : public IAcpProcessChannel
{
public:
    QByteArray written;
    explicit RecordingChannel(QObject *parent = nullptr) : IAcpProcessChannel(parent) {}
    void start() override { m_running = true; }
    void write(const QByteArray &bytes) override { written.append(bytes); }
    void kill() override { m_running = false; }
    bool isRunning() const override { return m_running; }
    void pushStdout(const QByteArray &chunk) { emit readyReadStdout(chunk); }

private:
    bool m_running = false;
};

QByteArray frame(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray update(const QString &kind, QJsonObject extra = {})
{
    extra.insert(QStringLiteral("sessionUpdate"), kind);
    return frame({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                  {QStringLiteral("method"), QStringLiteral("session/update")},
                  {QStringLiteral("params"), QJsonObject{{QStringLiteral("sessionId"), QStringLiteral("s1")},
                                                         {QStringLiteral("update"), extra}}}});
}

QByteArray agentText(const QString &text, const QString &messageId)
{
    return update(QStringLiteral("agent_message_chunk"),
                  {{QStringLiteral("content"), QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                           {QStringLiteral("text"), text}}},
                   {QStringLiteral("messageId"), messageId}});
}

// The usage_update claude-agent-acp sends at the SDK result of every model
// cycle; `origin` is "human" for a prompt, "task-notification" for a wake-up.
QByteArray cycleResult(const QString &origin)
{
    return update(QStringLiteral("usage_update"),
                  {{QStringLiteral("used"), 48850},
                   {QStringLiteral("size"), 1000000},
                   {QStringLiteral("cost"), QJsonObject{{QStringLiteral("amount"), 0.38},
                                                        {QStringLiteral("currency"), QStringLiteral("USD")}}},
                   {QStringLiteral("_meta"), QJsonObject{{QStringLiteral("_claude/origin"),
                                                          QJsonObject{{QStringLiteral("kind"), origin}}}}}});
}

QByteArray toolCall(const QString &kind, const QString &tool, const QString &id, QJsonObject extra)
{
    extra.insert(QStringLiteral("_meta"), QJsonObject{{QStringLiteral("claudeCode"),
                                                       QJsonObject{{QStringLiteral("toolName"), tool}}}});
    extra.insert(QStringLiteral("toolCallId"), id);
    return update(kind, extra);
}

QByteArray toolStart(const QString &tool, const QString &id)
{
    return toolCall(QStringLiteral("tool_call"), tool, id,
                    {{QStringLiteral("title"), tool},
                     {QStringLiteral("kind"), QStringLiteral("other")},
                     {QStringLiteral("status"), QStringLiteral("pending")},
                     {QStringLiteral("rawInput"), QJsonObject{}}});
}

QByteArray toolDone(const QString &tool, const QString &id, const QString &output,
                    const QJsonObject &rawInput = {})
{
    QJsonObject extra{{QStringLiteral("status"), QStringLiteral("completed")},
                      {QStringLiteral("rawOutput"), output},
                      {QStringLiteral("content"), QJsonArray{QJsonObject{
                           {QStringLiteral("type"), QStringLiteral("content")},
                           {QStringLiteral("content"), QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                   {QStringLiteral("text"), output}}}}}}};
    if (!rawInput.isEmpty())
        extra.insert(QStringLiteral("rawInput"), rawInput);
    return toolCall(QStringLiteral("tool_call_update"), tool, id, extra);
}

QByteArray monitorArmed(const QString &taskId)
{
    return toolDone(QStringLiteral("Monitor"), QStringLiteral("toolu_monitor"),
                    QStringLiteral("Monitor started (task %1, expires in 30m unless the source ends first). "
                                   "You will be notified on each event.").arg(taskId),
                    {{QStringLiteral("command"), QStringLiteral("npm run e2e")},
                     {QStringLiteral("description"), QStringLiteral("e2e steps")}});
}

void wire(AcpConnection *conn, AcpSessionModel *model)
{
    QObject::connect(conn, &AcpConnection::messageChunk, model,
                     qOverload<const QString &, const QString &>(&AcpSessionModel::onMessageChunk));
    QObject::connect(conn, &AcpConnection::thoughtChunk, model, &AcpSessionModel::onThoughtChunk);
    QObject::connect(conn, &AcpConnection::toolCallReceived, model, &AcpSessionModel::onToolCallReceived);
    QObject::connect(conn, &AcpConnection::toolCallUpdated, model, &AcpSessionModel::onToolCallUpdated);
    QObject::connect(conn, &AcpConnection::promptStarted, model, &AcpSessionModel::onPromptStarted);
    QObject::connect(conn, &AcpConnection::promptEnded, model, &AcpSessionModel::onPromptEnded);
    QObject::connect(conn, &AcpConnection::asyncTasksChanged, model, &AcpSessionModel::onAsyncTasksChanged);
}

// The transcript top to bottom, one line per visible message, card or fold row.
// A fold row reads "fold (live)" while its turn runs and "fold (waiting)" once
// the turn folded but background work it started is still alive.
QStringList transcript(AcpSessionView &view)
{
    QStringList out;
    QVBoxLayout *layout = nullptr;
    for (auto *scroll : view.findChildren<QScrollArea *>()) {
        if (scroll->widget() && (layout = qobject_cast<QVBoxLayout *>(scroll->widget()->layout())))
            break;
    }
    if (!layout)
        return {QStringLiteral("<no transcript>")};
    for (int i = 0; i < layout->count(); ++i) {
        QWidget *w = layout->itemAt(i)->widget();
        if (!w || w->isHidden())
            continue;
        if (auto *m = qobject_cast<AcpMessageWidget *>(w))
            out << QStringLiteral("%1: %2").arg(m->role(), m->plainText());
        else if (auto *c = qobject_cast<AcpToolCallCard *>(w))
            out << QStringLiteral("card %1").arg(c->id());
        else if (auto *r = qobject_cast<AcpTurnFoldRow *>(w))
            out << (r->isLiveInteractive() ? QStringLiteral("fold (waiting)")
                    : r->isLive()          ? QStringLiteral("fold (live)")
                                           : QStringLiteral("fold"));
    }
    return out;
}

// The visible status line text naming background work, empty when none shows.
QString statusLine(AcpSessionView &view)
{
    for (auto *label : view.findChildren<QLabel *>()) {
        if (!label->isHidden() && (label->text().contains(QStringLiteral("Monitor ("))
                                   || label->text().contains(QStringLiteral("Waiting background"))))
            return label->text();
    }
    return {};
}

// One session: connection -> model -> view, as AcpAgentManager wires them.
struct Session
{
    QTemporaryDir historyDir;
    AcpConnection conn;
    RecordingChannel *channel = nullptr;
    std::unique_ptr<AcpSessionModel> model;
    std::unique_ptr<AcpSessionView> view;

    Session()
    {
        channel = new RecordingChannel(&conn);
        conn.attachChannelForTest(channel);
        channel->start();
        conn.setSessionIdForTest(QStringLiteral("s1"));
        conn.setBackgroundWakeGraceMsForTest(100);
        model = std::make_unique<AcpSessionModel>(QStringLiteral("s1"), QStringLiteral("p"), historyDir.path());
        wire(&conn, model.get());
        view = std::make_unique<AcpSessionView>(model.get(), &conn, nullptr);
        view->resize(520, 900);
        view->show();
    }
    ~Session()
    {
        view.reset();
        model.reset();
    }

    // Sends a prompt; returns its JSON-RPC id.
    int prompt(const QString &text)
    {
        model->appendUserMessage(text, {});
        channel->written.clear();
        conn.sendPrompt(text, {});
        return QJsonDocument::fromJson(channel->written.trimmed()).object().value(QStringLiteral("id")).toInt();
    }

    void endPrompt(int id)
    {
        channel->pushStdout(cycleResult(QStringLiteral("human")));
        channel->pushStdout(frame({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                   {QStringLiteral("id"), id},
                                   {QStringLiteral("result"), QJsonObject{{QStringLiteral("stopReason"),
                                                                           QStringLiteral("end_turn")}}}}));
    }

    // A wake-up cycle: the agent acts on an event by itself, then the cycle ends.
    void wakeUp(const QList<QByteArray> &frames)
    {
        for (const QByteArray &f : frames)
            channel->pushStdout(f);
        channel->pushStdout(cycleResult(QStringLiteral("task-notification")));
    }
};

} // namespace

class TestAcpSessionViewWake : public QObject
{
    Q_OBJECT

private slots:
    void monitorEventsCloseTogether_foldEachWakeUp();
    void backgroundTask_keepsFoldedRowTickingUntilIdle();
};

// A Monitor tool streams its events to the model as wake-up turns (no
// session/prompt), often a second or two apart. Each one must fold back under
// the turn's row when its cycle ends, leaving only the newest reply visible;
// they used to pile up as one bubble per event while the turn stayed live.
// While the watch lives the row keeps ticking and the status line says so;
// TaskStop ends both.
void TestAcpSessionViewWake::monitorEventsCloseTogether_foldEachWakeUp()
{
    Session s;
    const int id = s.prompt(QStringLiteral("watch the e2e run"));
    s.channel->pushStdout(toolStart(QStringLiteral("Monitor"), QStringLiteral("toolu_monitor")));
    s.channel->pushStdout(monitorArmed(QStringLiteral("b6x8d7os3")));
    s.channel->pushStdout(agentText(QStringLiteral("armed"), QStringLiteral("msg_0")));
    s.endPrompt(id);
    QTRY_COMPARE(transcript(*s.view), (QStringList{QStringLiteral("user: watch the e2e run"),
                                                   QStringLiteral("fold (waiting)"),
                                                   QStringLiteral("assistant: armed")}));
    QVERIFY2(statusLine(*s.view).contains(QStringLiteral("Monitor (1)")), qPrintable(statusLine(*s.view)));

    // Two events 1 s apart, each answered in a wake-up cycle of its own.
    for (int tick = 1; tick <= 2; ++tick) {
        s.wakeUp({agentText(QStringLiteral("got tick %1").arg(tick), QStringLiteral("msg_%1").arg(tick))});
        QTest::qWait(1000);
    }
    QCOMPARE(transcript(*s.view), (QStringList{QStringLiteral("user: watch the e2e run"),
                                               QStringLiteral("fold (waiting)"),
                                               QStringLiteral("assistant: got tick 2")}));

    // The agent stops the watch: nothing is left to wake it, the clock stops.
    s.wakeUp({toolStart(QStringLiteral("TaskStop"), QStringLiteral("toolu_stop")),
              toolDone(QStringLiteral("TaskStop"), QStringLiteral("toolu_stop"),
                       QStringLiteral("{\"task_id\":\"b6x8d7os3\"}"),
                       {{QStringLiteral("task_id"), QStringLiteral("b6x8d7os3")}}),
              agentText(QStringLiteral("stopped"), QStringLiteral("msg_3"))});
    QTRY_COMPARE(transcript(*s.view), (QStringList{QStringLiteral("user: watch the e2e run"),
                                                   QStringLiteral("fold"),
                                                   QStringLiteral("assistant: stopped")}));
    QVERIFY(statusLine(*s.view).isEmpty());
}

// A background task outlives the prompt: the turn folds, its row keeps ticking
// (and still expands), and stops once the task ends with no wake-up after it.
void TestAcpSessionViewWake::backgroundTask_keepsFoldedRowTickingUntilIdle()
{
    Session s;
    const int id = s.prompt(QStringLiteral("start the server"));
    s.channel->pushStdout(toolStart(QStringLiteral("Bash"), QStringLiteral("toolu_t1")));
    s.channel->pushStdout(update(QStringLiteral("async_task_spawned"),
                                 {{QStringLiteral("asyncTaskId"), QStringLiteral("t1")},
                                  {QStringLiteral("name"), QStringLiteral("dev server")},
                                  {QStringLiteral("canStop"), true},
                                  {QStringLiteral("toolCallId"), QStringLiteral("toolu_t1")}}));
    s.channel->pushStdout(toolDone(QStringLiteral("Bash"), QStringLiteral("toolu_t1"), QStringLiteral("started")));
    s.channel->pushStdout(agentText(QStringLiteral("server is up"), QStringLiteral("msg_0")));
    s.endPrompt(id);
    QTRY_COMPARE(transcript(*s.view), (QStringList{QStringLiteral("user: start the server"),
                                                   QStringLiteral("fold (waiting)"),
                                                   QStringLiteral("assistant: server is up")}));
    QVERIFY(statusLine(*s.view).contains(QStringLiteral("Waiting background (1)")));

    auto *row = s.view->findChild<AcpTurnFoldRow *>();
    QVERIFY(row);
    row->click(); // expandable while it waits
    QVERIFY(row->isExpanded());
    QTRY_VERIFY(transcript(*s.view).contains(QStringLiteral("card toolu_t1")));
    row->click();
    QTRY_VERIFY(!transcript(*s.view).contains(QStringLiteral("card toolu_t1")));

    s.channel->pushStdout(update(QStringLiteral("async_task_state_update"),
                                 {{QStringLiteral("asyncTaskId"), QStringLiteral("t1")},
                                  {QStringLiteral("state"), QStringLiteral("completed")}}));
    QTRY_COMPARE(transcript(*s.view), (QStringList{QStringLiteral("user: start the server"),
                                                   QStringLiteral("fold"),
                                                   QStringLiteral("assistant: server is up")}));
}

QTEST_MAIN(TestAcpSessionViewWake)
#include "test_acp_session_view_wake.moc"
