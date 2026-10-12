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
#include <QString>
#include <QVector>

#include "AcpConversationCopy.h"
#include "AcpProtocol.h"
#include "AcpSessionModel.h"

class TestAcpConversationCopy : public QObject
{
    Q_OBJECT

private slots:
    void emptyTimeline();
    void reportIsLastAssistantMessageOfEachTurn();
    void runningTurnHasNoReportUntilItEnds();
    void blankAndJudgeMessagesAreNeverReports();
    void agentOnlyPrefixHasAReport();
    void conversationKeepsPromptsAndReportsOnly();
    void conversationStopsAtThePickedReport();
    void conversationSkipsEntriesRemovedFromTimeline();
    void conversationRendersImagesAsPlaceholders();
};

namespace {

struct Fx
{
    QVector<AcpTimelineEntry> tl;
    QVector<AcpMessage> msgs;

    int msg(const char *role, const QString &text, bool judge = false)
    {
        AcpMessage m;
        m.role = QString::fromLatin1(role);
        m.fromGoalJudge = judge;
        if (!text.isNull()) {
            AcpProtocol::AcpContentBlock b;
            b.kind = AcpProtocol::AcpContentBlock::Kind::Text;
            b.text = text;
            m.content.append(b);
        }
        msgs.append(m);
        AcpTimelineEntry e;
        e.kind = AcpTimelineEntry::Kind::Message;
        e.messageIndex = msgs.size() - 1;
        tl.append(e);
        return msgs.size() - 1;
    }

    void tool(const QString &id)
    {
        AcpTimelineEntry e;
        e.kind = AcpTimelineEntry::Kind::ToolCall;
        e.toolCallId = id;
        tl.append(e);
    }
};

} // namespace

void TestAcpConversationCopy::emptyTimeline()
{
    Fx f;
    QVERIFY(AcpConversationCopy::finalReports(f.tl, f.msgs, true).isEmpty());
    QCOMPARE(AcpConversationCopy::buildMarkdown(f.tl, f.msgs, 0), QString());
}

void TestAcpConversationCopy::reportIsLastAssistantMessageOfEachTurn()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    f.msg("assistant", QStringLiteral("looking"));
    f.tool(QStringLiteral("t1"));
    const int r1 = f.msg("assistant", QStringLiteral("report 1"));
    f.msg("thought", QStringLiteral("hmm"));
    f.msg("user", QStringLiteral("q2"));
    const int r2 = f.msg("assistant", QStringLiteral("report 2"));
    f.tool(QStringLiteral("t2")); // a trailing tool call does not displace the report

    QCOMPARE(AcpConversationCopy::finalReports(f.tl, f.msgs, true), (QVector<int>{r1, r2}));
}

void TestAcpConversationCopy::runningTurnHasNoReportUntilItEnds()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    const int r1 = f.msg("assistant", QStringLiteral("report 1"));
    f.msg("user", QStringLiteral("q2"));
    const int r2 = f.msg("assistant", QStringLiteral("interim"));

    QCOMPARE(AcpConversationCopy::finalReports(f.tl, f.msgs, false), (QVector<int>{r1}));
    QCOMPARE(AcpConversationCopy::finalReports(f.tl, f.msgs, true), (QVector<int>{r1, r2}));
}

void TestAcpConversationCopy::blankAndJudgeMessagesAreNeverReports()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    const int r1 = f.msg("assistant", QStringLiteral("report"));
    f.msg("assistant", QStringLiteral("  \n"));
    f.msg("system", QStringLiteral("Judging goal"));
    f.msg("assistant", QStringLiteral("<action>continue</action>"), true);
    f.msg("user", QStringLiteral("q2"));
    f.msg("assistant", QString()); // no content at all

    QCOMPARE(AcpConversationCopy::finalReports(f.tl, f.msgs, true), (QVector<int>{r1}));
}

void TestAcpConversationCopy::agentOnlyPrefixHasAReport()
{
    Fx f;
    const int r0 = f.msg("assistant", QStringLiteral("hello"));
    f.msg("user", QStringLiteral("q1"));

    QCOMPARE(AcpConversationCopy::finalReports(f.tl, f.msgs, false), (QVector<int>{r0}));
}

void TestAcpConversationCopy::conversationKeepsPromptsAndReportsOnly()
{
    Fx f;
    f.msg("user", QStringLiteral("  q1\n"));
    f.msg("thought", QStringLiteral("thinking"));
    f.msg("assistant", QStringLiteral("let me look"));
    f.tool(QStringLiteral("t1"));
    f.msg("assistant", QStringLiteral("report 1\n\n"));
    f.msg("system", QStringLiteral("Compacting completed."));
    f.msg("user", QStringLiteral("q2"));
    const int r2 = f.msg("assistant", QStringLiteral("report 2"));

    QCOMPARE(AcpConversationCopy::buildMarkdown(f.tl, f.msgs, r2),
             QStringLiteral("## User\n\nq1\n\n"
                            "## Assistant\n\nreport 1\n\n"
                            "## User\n\nq2\n\n"
                            "## Assistant\n\nreport 2"));
}

void TestAcpConversationCopy::conversationStopsAtThePickedReport()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    f.msg("assistant", QStringLiteral("interim"));
    const int r1 = f.msg("assistant", QStringLiteral("report 1"));
    f.msg("user", QStringLiteral("q2"));
    f.msg("assistant", QStringLiteral("report 2"));

    QCOMPARE(AcpConversationCopy::buildMarkdown(f.tl, f.msgs, r1),
             QStringLiteral("## User\n\nq1\n\n## Assistant\n\nreport 1"));
}

void TestAcpConversationCopy::conversationSkipsEntriesRemovedFromTimeline()
{
    Fx f;
    f.msg("user", QStringLiteral("q1"));
    const int r1 = f.msg("assistant", QStringLiteral("report 1"));
    // A finished goal-judge turn leaves its messages in the model but drops
    // their timeline entries: the copy follows the timeline.
    const int orphan = f.msg("assistant", QStringLiteral("judge verdict"));
    f.tl.removeLast();

    QCOMPARE(AcpConversationCopy::finalReports(f.tl, f.msgs, true), (QVector<int>{r1}));
    QCOMPARE(AcpConversationCopy::buildMarkdown(f.tl, f.msgs, r1),
             QStringLiteral("## User\n\nq1\n\n## Assistant\n\nreport 1"));
    QCOMPARE(AcpConversationCopy::buildMarkdown(f.tl, f.msgs, orphan), QString());
}

void TestAcpConversationCopy::conversationRendersImagesAsPlaceholders()
{
    Fx f;
    const int q = f.msg("user", QStringLiteral("see this:"));
    AcpProtocol::AcpContentBlock img;
    img.kind = AcpProtocol::AcpContentBlock::Kind::Image;
    img.imageData = QByteArrayLiteral("png");
    f.msgs[q].content.append(img);
    const int r = f.msg("assistant", QStringLiteral("nice"));

    QCOMPARE(AcpConversationCopy::buildMarkdown(f.tl, f.msgs, r),
             QStringLiteral("## User\n\nsee this:[image]\n\n## Assistant\n\nnice"));
}

QTEST_APPLESS_MAIN(TestAcpConversationCopy)
#include "test_acp_conversation_copy.moc"
