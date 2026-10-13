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
#include <QHash>
#include <QString>
#include <QVector>

#include "AcpProtocol.h"
#include "AcpSessionModel.h"
#include "AcpTurnFolding.h"

class TestAcpTurnFolding : public QObject
{
    Q_OBJECT

private slots:
    void emptyTimeline();
    void singleTurnIsNeverFolded();
    void includeNewestReturnsNewestTurn();
    void finishedTurnFoldsToolsAndEarlierMessages();
    void survivorIsLastAssistantMessageNotTrailingTool();
    void turnWithOnlyReportHasNothingToFold();
    void turnWithoutAssistantMessageFoldsEverything();
    void userAndSystemStayPinned();
    void thoughtsFold();
    void emptyAssistantBubbleDoesNotCountAsSurvivor();
    void goalMessageStartsNewTurn();
    void judgingMarkerDoesNotSplitTurn();
    void workedMsIsAnchorToSurvivor();
    void workedMsUnknownWithoutTimestamps();
    void agentOnlyPrefixIsATurn();
    void unknownToolCallIsIgnored();
    void newestTurnBeginFindsLastPrompt();
    void toolCallsAreCounted();
    void lateAttributedSubagentCallIsSkipped();
    void reportOnlyTurnKeepsDurationRow();
};

namespace {

struct Fx
{
    QVector<AcpMessage> msgs;
    QVector<AcpTimelineEntry> tl;
    QHash<QString, AcpProtocol::AcpToolCall> tools;

    int msg(const char *role, const QString &text, qint64 ts = 0, bool goal = false)
    {
        AcpMessage m;
        m.role = QLatin1String(role);
        m.fromGoalAgent = goal;
        m.timestamp = ts;
        AcpProtocol::AcpContentBlock b;
        b.kind = AcpProtocol::AcpContentBlock::Kind::Text;
        b.text = text;
        m.content.append(b);
        msgs.append(m);
        AcpTimelineEntry e;
        e.kind = AcpTimelineEntry::Kind::Message;
        e.messageIndex = msgs.size() - 1;
        tl.append(e);
        return tl.size() - 1;
    }
    int tool(const QString &id, bool known = true)
    {
        if (known) {
            AcpProtocol::AcpToolCall tc;
            tc.id = id;
            tools.insert(id, tc);
        }
        AcpTimelineEntry e;
        e.kind = AcpTimelineEntry::Kind::ToolCall;
        e.toolCallId = id;
        tl.append(e);
        return tl.size() - 1;
    }
    QVector<AcpTurnFolding::Turn> run() const
    {
        return AcpTurnFolding::compute(tl, msgs, tools);
    }
};

} // namespace

void TestAcpTurnFolding::emptyTimeline()
{
    Fx f;
    QVERIFY(f.run().isEmpty());
}

void TestAcpTurnFolding::singleTurnIsNeverFolded()
{
    Fx f;
    f.msg("user", QStringLiteral("go"));
    f.tool(QStringLiteral("t1"));
    f.msg("assistant", QStringLiteral("done"));
    QVERIFY(f.run().isEmpty());
}

void TestAcpTurnFolding::includeNewestReturnsNewestTurn()
{
    Fx f;
    f.msg("user", QStringLiteral("go"), 1000);
    const int t1 = f.tool(QStringLiteral("t1"));
    const int rep = f.msg("assistant", QStringLiteral("done"), 4000);

    const auto turns = AcpTurnFolding::compute(f.tl, f.msgs, f.tools, true);
    QCOMPARE(turns.size(), 1);
    QCOMPARE(turns.at(0).end, f.tl.size());
    QCOMPARE(turns.at(0).survivor, rep);
    QCOMPARE(turns.at(0).folded, (QVector<int>{t1}));
    QCOMPARE(turns.at(0).workedMs, qint64(3000));
}

void TestAcpTurnFolding::finishedTurnFoldsToolsAndEarlierMessages()
{
    Fx f;
    const int a = f.msg("user", QStringLiteral("q1"));
    const int m1 = f.msg("assistant", QStringLiteral("let me look"));
    const int t1 = f.tool(QStringLiteral("t1"));
    const int t2 = f.tool(QStringLiteral("t2"));
    const int rep = f.msg("assistant", QStringLiteral("report"));
    f.msg("user", QStringLiteral("q2"));

    const auto turns = f.run();
    QCOMPARE(turns.size(), 1);
    QCOMPARE(turns.at(0).anchor, a);
    QCOMPARE(turns.at(0).survivor, rep);
    QCOMPARE(turns.at(0).folded, (QVector<int>{m1, t1, t2}));
    QCOMPARE(turns.at(0).begin, 0);
    QCOMPARE(turns.at(0).end, 5);
}

void TestAcpTurnFolding::survivorIsLastAssistantMessageNotTrailingTool()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    const int rep = f.msg("assistant", QStringLiteral("report"));
    const int tail = f.tool(QStringLiteral("late"));
    f.msg("user", QStringLiteral("q2"));

    const auto turns = f.run();
    QCOMPARE(turns.size(), 1);
    QCOMPARE(turns.at(0).survivor, rep);
    QCOMPARE(turns.at(0).folded, (QVector<int>{tail}));
}

void TestAcpTurnFolding::turnWithOnlyReportHasNothingToFold()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    f.msg("assistant", QStringLiteral("just an answer"));
    f.msg("user", QStringLiteral("q2"));
    f.tool(QStringLiteral("t"));
    QVERIFY(f.run().isEmpty());
}

void TestAcpTurnFolding::turnWithoutAssistantMessageFoldsEverything()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    const int t1 = f.tool(QStringLiteral("t1"));
    f.msg("user", QStringLiteral("q2"));

    const auto turns = f.run();
    QCOMPARE(turns.size(), 1);
    QCOMPARE(turns.at(0).survivor, -1);
    QCOMPARE(turns.at(0).folded, (QVector<int>{t1}));
}

void TestAcpTurnFolding::userAndSystemStayPinned()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    const int t1 = f.tool(QStringLiteral("t1"));
    f.msg("system", QStringLiteral("notice"));
    const int rep = f.msg("assistant", QStringLiteral("report"));
    f.msg("user", QStringLiteral("q2"));

    const auto turns = f.run();
    QCOMPARE(turns.size(), 1);
    QCOMPARE(turns.at(0).folded, (QVector<int>{t1}));
    QCOMPARE(turns.at(0).survivor, rep);
}

void TestAcpTurnFolding::thoughtsFold()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    const int th = f.msg("thought", QStringLiteral("hmm"));
    f.msg("assistant", QStringLiteral("report"));
    f.msg("user", QStringLiteral("q2"));

    const auto turns = f.run();
    QCOMPARE(turns.size(), 1);
    QCOMPARE(turns.at(0).folded, (QVector<int>{th}));
}

void TestAcpTurnFolding::emptyAssistantBubbleDoesNotCountAsSurvivor()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    const int rep = f.msg("assistant", QStringLiteral("report"));
    f.msg("assistant", QStringLiteral("   "));
    const int t = f.tool(QStringLiteral("t"));
    f.msg("user", QStringLiteral("q2"));

    const auto turns = f.run();
    QCOMPARE(turns.size(), 1);
    QCOMPARE(turns.at(0).survivor, rep);
    QCOMPARE(turns.at(0).folded, (QVector<int>{t}));
}

void TestAcpTurnFolding::goalMessageStartsNewTurn()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    const int t1 = f.tool(QStringLiteral("t1"));
    f.msg("assistant", QStringLiteral("r1"));
    f.msg("user", QStringLiteral("goal prompt"), 0, true);
    const int t2 = f.tool(QStringLiteral("t2"));
    f.msg("assistant", QStringLiteral("r2"));
    f.msg("user", QStringLiteral("q3"));

    const auto turns = f.run();
    QCOMPARE(turns.size(), 2);
    QCOMPARE(turns.at(0).folded, (QVector<int>{t1}));
    QCOMPARE(turns.at(1).folded, (QVector<int>{t2}));
}

void TestAcpTurnFolding::judgingMarkerDoesNotSplitTurn()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    f.tool(QStringLiteral("t1"));
    f.msg("assistant", QStringLiteral("report"));
    const int sys = f.msg("system", QStringLiteral("judging"));
    f.msgs[f.tl.at(sys).messageIndex].marker = QLatin1String(kAcpMarkerGoalJudging);
    f.tool(QStringLiteral("judge-tool"));
    f.msg("assistant", QStringLiteral("verdict"));
    // Still a single (newest) turn: nothing folds until a real prompt arrives.
    QVERIFY(f.run().isEmpty());
}

void TestAcpTurnFolding::workedMsIsAnchorToSurvivor()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"), 1000);
    f.tool(QStringLiteral("t1"));
    f.msg("assistant", QStringLiteral("report"), 17000);
    f.msg("user", QStringLiteral("q2"), 20000);

    const auto turns = f.run();
    QCOMPARE(turns.size(), 1);
    QCOMPARE(turns.at(0).workedMs, qint64(16000));
}

void TestAcpTurnFolding::workedMsUnknownWithoutTimestamps()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"), 0);
    f.tool(QStringLiteral("t1"));
    f.msg("assistant", QStringLiteral("report"), 0);
    f.msg("user", QStringLiteral("q2"), 0);
    QCOMPARE(f.run().at(0).workedMs, qint64(-1));

    Fx g; // negative span (clock skew) is unknown, not a negative duration
    g.msg("user", QStringLiteral("q1"), 9000);
    g.tool(QStringLiteral("t1"));
    g.msg("assistant", QStringLiteral("report"), 1000);
    g.msg("user", QStringLiteral("q2"), 10000);
    QCOMPARE(g.run().at(0).workedMs, qint64(-1));
}

void TestAcpTurnFolding::agentOnlyPrefixIsATurn()
{
    Fx f;
    const int t = f.tool(QStringLiteral("t0"));
    const int rep = f.msg("assistant", QStringLiteral("hello"));
    f.msg("user", QStringLiteral("q1"));

    const auto turns = f.run();
    QCOMPARE(turns.size(), 1);
    QCOMPARE(turns.at(0).anchor, -1);
    QCOMPARE(turns.at(0).survivor, rep);
    QCOMPARE(turns.at(0).folded, (QVector<int>{t}));
}

void TestAcpTurnFolding::unknownToolCallIsIgnored()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    f.tool(QStringLiteral("ghost"), false); // not in the tool-call table: renders nothing
    f.msg("assistant", QStringLiteral("report"));
    f.msg("user", QStringLiteral("q2"));
    QVERIFY(f.run().isEmpty());
}

void TestAcpTurnFolding::newestTurnBeginFindsLastPrompt()
{
    Fx f;
    QCOMPARE(AcpTurnFolding::newestTurnBegin(f.tl, f.msgs), -1);

    f.tool(QStringLiteral("t0"));
    f.msg("assistant", QStringLiteral("hello"));
    QCOMPARE(AcpTurnFolding::newestTurnBegin(f.tl, f.msgs), 0); // agent-only prefix

    f.msg("user", QStringLiteral("q1"));
    const int q2 = f.msg("user", QStringLiteral("q2"));
    f.tool(QStringLiteral("t1"));
    f.msg("assistant", QStringLiteral("r2"));
    QCOMPARE(AcpTurnFolding::newestTurnBegin(f.tl, f.msgs), q2);
}

void TestAcpTurnFolding::toolCallsAreCounted()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    f.msg("assistant", QStringLiteral("looking"));
    f.tool(QStringLiteral("t1"));
    f.tool(QStringLiteral("t2"));
    f.tool(QStringLiteral("ghost"), false); // unknown: renders nothing, not counted
    f.msg("assistant", QStringLiteral("report"));
    f.msg("user", QStringLiteral("q2"));

    const auto turns = f.run();
    QCOMPARE(turns.size(), 1);
    QCOMPARE(turns.at(0).toolCalls, 2);
}

void TestAcpTurnFolding::lateAttributedSubagentCallIsSkipped()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    const int agent = f.tool(QStringLiteral("agent"));
    // Got its entry before the adapter named its subagent: it renders inside
    // the Agent card, so the turn neither folds nor counts it.
    f.tool(QStringLiteral("child"));
    f.tools[QStringLiteral("child")].parentToolCallId = QStringLiteral("agent");
    f.msg("assistant", QStringLiteral("report"));
    f.msg("user", QStringLiteral("q2"));

    const auto turns = f.run();
    QCOMPARE(turns.size(), 1);
    QCOMPARE(turns.at(0).folded, (QVector<int>{agent}));
    QCOMPARE(turns.at(0).toolCalls, 1);
}

void TestAcpTurnFolding::reportOnlyTurnKeepsDurationRow()
{
    Fx f;
    f.msg("user", QStringLiteral("hi"), 1000);
    const int rep = f.msg("assistant", QStringLiteral("hello"), 3000);
    f.msg("user", QStringLiteral("q2"), 9000);

    const auto turns = f.run();
    QCOMPARE(turns.size(), 1);
    QVERIFY(turns.at(0).folded.isEmpty());
    QCOMPARE(turns.at(0).survivor, rep);
    QCOMPARE(turns.at(0).workedMs, qint64(2000));
    QCOMPARE(turns.at(0).toolCalls, 0);
}

QTEST_APPLESS_MAIN(TestAcpTurnFolding)
#include "test_acp_turn_folding.moc"
