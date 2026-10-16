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

#include "AcpSwitchAgent.h"

#include <QtTest>

using AcpSwitchAgent::Block;

namespace {

AcpAgentDefinition agent(const char *id, const char *name, const char *command, QStringList args)
{
    AcpAgentDefinition def;
    def.id = QString::fromLatin1(id);
    def.name = QString::fromLatin1(name);
    def.command = QString::fromLatin1(command);
    def.args = std::move(args);
    return def;
}

AcpAgentDefinition claudeAccount(const char *id, const char *name)
{
    return agent(id, name, "npx", {QStringLiteral("-y"), QStringLiteral("@agentclientprotocol/claude-agent-acp@latest")});
}

AcpMessage message(const char *role, qint64 ts, bool fromGoalJudge = false)
{
    AcpMessage m;
    m.role = QString::fromLatin1(role);
    m.timestamp = ts;
    m.fromGoalJudge = fromGoalJudge;
    return m;
}

} // namespace

class TestAcpSwitchAgent : public QObject
{
    Q_OBJECT

private slots:
    void idleSessionIsNotBlocked();
    void eachKindOfWorkBlocks_data();
    void eachKindOfWorkBlocks();
    void remoteAndForeignAgentWinOverWork();
    void targets_onlyOtherClaudeCodeAgentsInRegistryOrder();
    void sessionNewest_ignoresSystemNoticesAndJudge();
    void conversation_needsAPromptNotJustNotices();
    void freshness_allowsSmallClockSlackOnly();
};

void TestAcpSwitchAgent::idleSessionIsNotBlocked()
{
    AcpSwitchAgent::SessionState s;
    s.sourceIsClaudeCode = true;
    QCOMPARE(AcpSwitchAgent::blockReason(s), Block::None);
}

void TestAcpSwitchAgent::eachKindOfWorkBlocks_data()
{
    QTest::addColumn<int>("field");
    QTest::addColumn<int>("expected");
    QTest::newRow("switching") << 0 << int(Block::Switching);
    QTest::newRow("turn") << 1 << int(Block::TurnRunning);
    QTest::newRow("goal") << 2 << int(Block::GoalActive);
    QTest::newRow("waiting") << 3 << int(Block::WaitingForUser);
    QTest::newRow("background") << 4 << int(Block::BackgroundWork);
    QTest::newRow("queued") << 5 << int(Block::PromptsQueued);
}

void TestAcpSwitchAgent::eachKindOfWorkBlocks()
{
    QFETCH(int, field);
    QFETCH(int, expected);
    AcpSwitchAgent::SessionState s;
    s.sourceIsClaudeCode = true;
    switch (field) {
    case 0: s.switching = true; break;
    case 1: s.turnRunning = true; break;
    case 2: s.goalActive = true; break;
    case 3: s.waitingForUser = true; break;
    case 4: s.backgroundWork = true; break;
    case 5: s.queuedPrompts = 2; break;
    default: QFAIL("bad row");
    }
    QCOMPARE(int(AcpSwitchAgent::blockReason(s)), expected);
}

void TestAcpSwitchAgent::remoteAndForeignAgentWinOverWork()
{
    // These say "never here", not "not now": the menu lists no target at all.
    AcpSwitchAgent::SessionState s;
    s.sourceIsClaudeCode = true;
    s.turnRunning = true;
    s.remote = true;
    QCOMPARE(AcpSwitchAgent::blockReason(s), Block::Remote);
    s.remote = false;
    s.sourceIsClaudeCode = false;
    QCOMPARE(AcpSwitchAgent::blockReason(s), Block::SourceNotClaudeCode);
}

void TestAcpSwitchAgent::targets_onlyOtherClaudeCodeAgentsInRegistryOrder()
{
    const QList<AcpAgentDefinition> all{
        claudeAccount("cc-a", "CC nullmastermind"),
        agent("codex", "Codex", "npx", {QStringLiteral("@zed-industries/codex-acp")}),
        claudeAccount("cc-b", "CC 0x317374"),
        claudeAccount("cc-c", "CC muse"),
    };
    QVERIFY(AcpSwitchAgent::isClaudeCodeAgent(all.at(0)));
    QVERIFY(!AcpSwitchAgent::isClaudeCodeAgent(all.at(1)));
    const QList<AcpAgentDefinition> targets = AcpSwitchAgent::claudeCodeAgents(all);
    QCOMPARE(targets.size(), 3);
    QCOMPARE(targets.at(0).id, QStringLiteral("cc-a"));
    QCOMPARE(targets.at(1).id, QStringLiteral("cc-b"));
    QCOMPARE(targets.at(2).id, QStringLiteral("cc-c"));
}

void TestAcpSwitchAgent::sessionNewest_ignoresSystemNoticesAndJudge()
{
    const QVector<AcpMessage> msgs{
        message("user", 1000),
        message("assistant", 2000),
        message("thought", 1500),
        message("assistant", 9000, /*fromGoalJudge=*/true), // judge runs its own session
        message("system", 8000),                            // host notice, never in the file
    };
    QCOMPARE(AcpSwitchAgent::newestConversationMs(msgs), qint64(2000));
    QCOMPARE(AcpSwitchAgent::newestConversationMs({}), qint64(-1));
}

void TestAcpSwitchAgent::conversation_needsAPromptNotJustNotices()
{
    // A draft that only collected host notices ("Switched from X to Y") has
    // nothing Claude Code saved: it must start fresh, not resume.
    QVERIFY(!AcpSwitchAgent::hasConversation({message("system", 1000)}));
    QVERIFY(!AcpSwitchAgent::hasConversation({message("user", 1000, /*fromGoalJudge=*/true)}));
    QVERIFY(AcpSwitchAgent::hasConversation({message("system", 1000), message("user", 2000)}));
}

void TestAcpSwitchAgent::freshness_allowsSmallClockSlackOnly()
{
    // The transcript's newest record is written after the host stamps the
    // message (streaming starts first); "clear context" leaves it far behind.
    QVERIFY(AcpSwitchAgent::transcriptIsCurrent(10'000, 9'000));
    QVERIFY(AcpSwitchAgent::transcriptIsCurrent(10'000, 11'500));
    QVERIFY(!AcpSwitchAgent::transcriptIsCurrent(10'000, 60'000));
    QVERIFY(!AcpSwitchAgent::transcriptIsCurrent(-1, 1'000));
    QVERIFY(AcpSwitchAgent::transcriptIsCurrent(5'000, -1));
}

QTEST_GUILESS_MAIN(TestAcpSwitchAgent)
#include "test_acp_switch_agent.moc"
