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

#include "AcpPrewalk.h"

#include <QtTest>

using Selection = AcpPrewalk::Selection;

namespace {

const Selection kOpusHigh{QStringLiteral("opus"), QStringLiteral("high")};
const Selection kSonnetHigh{QStringLiteral("sonnet"), QStringLiteral("high")};

AcpProtocol::AcpToolCall call(const char *kind, const char *title, const char *parent = "")
{
    AcpProtocol::AcpToolCall tc;
    tc.id = QStringLiteral("t1");
    tc.kind = QString::fromLatin1(kind);
    tc.title = QString::fromLatin1(title);
    tc.parentToolCallId = QString::fromLatin1(parent);
    return tc;
}

AcpPrewalk::ToolCall edit()
{
    return AcpPrewalk::classify(call("edit", "Edit src/a.cpp"), false);
}

// Prewalk on Sonnet, a turn running on Opus/high.
AcpPrewalk runningTurn()
{
    AcpPrewalk p;
    p.setTarget(QStringLiteral("sonnet"));
    p.onTurnStarted();
    return p;
}

} // namespace

class TestAcpPrewalk : public QObject
{
    Q_OBJECT

private slots:
    void firstEditSwitchesToPrewalkModel();
    void laterEditsInTheTurnDoNotSwitchAgain();
    void onlyFileEditsCount_data();
    void onlyFileEditsCount();
    void subagentAndJudgeEditsDoNotCount();
    void offNeverSwitches();
    void alreadyOnPrewalkModelChangesNothing();
    void editOutsideATurnIsIgnored();
    void turnEndRestoresTheSessionSelection();
    void nextTurnStartsOnTheSessionModelAndSwitchesAgain();
    void userPickDuringSwitchWins();
    void userPickWhileRestoringStopsTheEffortCheck();
    void effortChangedByTheSwitchIsSetAgain();
    void effortKeptByTheSwitchIsLeftAlone();
    void userEffortPickDuringSwitchIsKept();
    void reportsWhileSwitchedAreNotARestore();
    void turningOffMidSwitchStillRestores();
    void sessionSelectionIsKnownUntilTheRestoreLands();
    void editBeforeTheRestoreLandsKeepsTheSessionSelection();
    void resetForgetsEverything();
};

void TestAcpPrewalk::firstEditSwitchesToPrewalkModel()
{
    AcpPrewalk p = runningTurn();
    QCOMPARE(p.onToolCall(edit(), kOpusHigh), QStringLiteral("sonnet"));
    QVERIFY(p.isSwitched());
}

void TestAcpPrewalk::laterEditsInTheTurnDoNotSwitchAgain()
{
    AcpPrewalk p = runningTurn();
    QCOMPARE(p.onToolCall(edit(), kOpusHigh), QStringLiteral("sonnet"));
    QCOMPARE(p.onToolCall(edit(), kSonnetHigh), QString());
    QCOMPARE(p.onToolCall(edit(), kSonnetHigh), QString());
}

void TestAcpPrewalk::onlyFileEditsCount_data()
{
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("title");
    QTest::addColumn<bool>("switches");

    QTest::newRow("edit") << "edit" << "Edit src/a.cpp" << true;
    QTest::newRow("write is kind edit") << "edit" << "Write b.txt" << true;
    QTest::newRow("delete") << "delete" << "Delete old.txt" << true;
    QTest::newRow("move") << "move" << "Move a -> b" << true;
    // A read of a file whose name says "edit" is still a read.
    QTest::newRow("read named edit") << "read" << "Read src/AcpEditor.cpp" << false;
    // Bash doubles as exploration, even when it writes.
    QTest::newRow("bash writing") << "execute" << "echo hi > write.txt" << false;
    QTest::newRow("search") << "search" << "grep apply_patch" << false;
    // No kind (some agents): the name/title needle decides.
    QTest::newRow("kindless write") << "" << "Write a.txt" << true;
    QTest::newRow("kindless apply_patch") << "" << "apply_patch" << true;
    QTest::newRow("kindless read") << "" << "Read a.txt" << false;
}

void TestAcpPrewalk::onlyFileEditsCount()
{
    QFETCH(QString, kind);
    QFETCH(QString, title);
    QFETCH(bool, switches);

    AcpPrewalk p = runningTurn();
    AcpProtocol::AcpToolCall tc;
    tc.kind = kind;
    tc.title = title;
    const QString to = p.onToolCall(AcpPrewalk::classify(tc, false), kOpusHigh);
    QCOMPARE(to, switches ? QStringLiteral("sonnet") : QString());
}

void TestAcpPrewalk::subagentAndJudgeEditsDoNotCount()
{
    AcpPrewalk p = runningTurn();
    QCOMPARE(p.onToolCall(AcpPrewalk::classify(call("edit", "Edit a.cpp", "agent-1"), false), kOpusHigh),
             QString());
    QCOMPARE(p.onToolCall(AcpPrewalk::classify(call("edit", "Edit a.cpp"), true), kOpusHigh), QString());
    QVERIFY(!p.isSwitched());
    // The main agent's own edit still switches afterwards.
    QCOMPARE(p.onToolCall(edit(), kOpusHigh), QStringLiteral("sonnet"));
}

void TestAcpPrewalk::offNeverSwitches()
{
    AcpPrewalk p;
    p.onTurnStarted();
    QCOMPARE(p.onToolCall(edit(), kOpusHigh), QString());
    QVERIFY(!p.onTurnEnded().has_value());
}

void TestAcpPrewalk::alreadyOnPrewalkModelChangesNothing()
{
    AcpPrewalk p = runningTurn();
    QCOMPARE(p.onToolCall(edit(), kSonnetHigh), QString());
    QVERIFY(!p.isSwitched());
    QVERIFY(!p.onTurnEnded().has_value());
}

void TestAcpPrewalk::editOutsideATurnIsIgnored()
{
    AcpPrewalk p;
    p.setTarget(QStringLiteral("sonnet"));
    // A late update of a finished turn's edit.
    QCOMPARE(p.onToolCall(edit(), kOpusHigh), QString());

    p.onTurnStarted();
    QVERIFY(!p.onTurnEnded().has_value());
    QCOMPARE(p.onToolCall(edit(), kOpusHigh), QString());
}

void TestAcpPrewalk::turnEndRestoresTheSessionSelection()
{
    AcpPrewalk p = runningTurn();
    p.onToolCall(edit(), kOpusHigh);

    const std::optional<Selection> restore = p.onTurnEnded();
    QVERIFY(restore.has_value());
    QCOMPARE(restore->model, QStringLiteral("opus"));
    QCOMPARE(restore->effort, QStringLiteral("high"));
    // Only once.
    QVERIFY(!p.onTurnEnded().has_value());
}

void TestAcpPrewalk::nextTurnStartsOnTheSessionModelAndSwitchesAgain()
{
    AcpPrewalk p = runningTurn();
    p.onToolCall(edit(), kOpusHigh);
    p.onTurnEnded();
    p.onSelectionReported(kOpusHigh);
    QVERIFY(!p.isSwitched());

    p.onTurnStarted();
    QCOMPARE(p.onToolCall(edit(), kOpusHigh), QStringLiteral("sonnet"));
    QCOMPARE(p.onTurnEnded()->model, QStringLiteral("opus"));
}

void TestAcpPrewalk::userPickDuringSwitchWins()
{
    AcpPrewalk p = runningTurn();
    p.onToolCall(edit(), kOpusHigh);
    p.onUserPickedModel();

    QVERIFY(!p.isSwitched());
    QVERIFY(!p.onTurnEnded().has_value());
}

void TestAcpPrewalk::userPickWhileRestoringStopsTheEffortCheck()
{
    AcpPrewalk p = runningTurn();
    p.onToolCall(edit(), kOpusHigh);
    p.onTurnEnded();
    p.onUserPickedModel();

    QCOMPARE(p.onSelectionReported({QStringLiteral("opus"), QStringLiteral("low")}), QString());
    QVERIFY(!p.sessionSelection().has_value());
}

void TestAcpPrewalk::effortChangedByTheSwitchIsSetAgain()
{
    AcpPrewalk p = runningTurn();
    p.onToolCall(edit(), kOpusHigh);
    p.onTurnEnded();

    // The agent reset effort when the model changed.
    QCOMPARE(p.onSelectionReported({QStringLiteral("opus"), QStringLiteral("medium")}),
             QStringLiteral("high"));
    QVERIFY(!p.isSwitched());
    QCOMPARE(p.onSelectionReported({QStringLiteral("opus"), QStringLiteral("medium")}), QString());
}

void TestAcpPrewalk::effortKeptByTheSwitchIsLeftAlone()
{
    AcpPrewalk p = runningTurn();
    p.onToolCall(edit(), kOpusHigh);
    p.onTurnEnded();
    QCOMPARE(p.onSelectionReported(kOpusHigh), QString());
    QVERIFY(!p.isSwitched());

    // An agent without an effort option.
    p.onTurnStarted();
    p.onToolCall(edit(), {QStringLiteral("opus"), QString()});
    p.onTurnEnded();
    QCOMPARE(p.onSelectionReported({QStringLiteral("opus"), QStringLiteral("medium")}), QString());
}

void TestAcpPrewalk::userEffortPickDuringSwitchIsKept()
{
    AcpPrewalk p = runningTurn();
    p.onToolCall(edit(), kOpusHigh);
    p.onUserPickedEffort(QStringLiteral("low"));
    QCOMPARE(p.sessionSelection()->effort, QStringLiteral("low"));

    const std::optional<Selection> restore = p.onTurnEnded();
    QCOMPARE(restore->model, QStringLiteral("opus"));
    QCOMPARE(restore->effort, QStringLiteral("low"));
    // Back on Opus with the agent's reset effort: the user's pick is set again.
    QCOMPARE(p.onSelectionReported({QStringLiteral("opus"), QStringLiteral("medium")}), QStringLiteral("low"));

    // Not switched: nothing to remember.
    p.onUserPickedEffort(QStringLiteral("high"));
    QVERIFY(!p.sessionSelection().has_value());
}

void TestAcpPrewalk::reportsWhileSwitchedAreNotARestore()
{
    AcpPrewalk p = runningTurn();
    p.onToolCall(edit(), kOpusHigh);
    QCOMPARE(p.onSelectionReported(kSonnetHigh), QString());
    QCOMPARE(p.onSelectionReported(kOpusHigh), QString());
    QVERIFY(p.isSwitched());

    p.onTurnEnded();
    // The restore has not landed yet: still on Sonnet.
    QCOMPARE(p.onSelectionReported({QStringLiteral("sonnet"), QStringLiteral("low")}), QString());
    QVERIFY(p.isSwitched());
}

void TestAcpPrewalk::turningOffMidSwitchStillRestores()
{
    AcpPrewalk p = runningTurn();
    p.onToolCall(edit(), kOpusHigh);
    p.setTarget(QString());

    QCOMPARE(p.onTurnEnded()->model, QStringLiteral("opus"));
    p.onTurnStarted();
    QCOMPARE(p.onToolCall(edit(), kOpusHigh), QString());
}

void TestAcpPrewalk::sessionSelectionIsKnownUntilTheRestoreLands()
{
    AcpPrewalk p = runningTurn();
    QVERIFY(!p.sessionSelection().has_value());

    p.onToolCall(edit(), kOpusHigh);
    QCOMPARE(p.sessionSelection()->model, QStringLiteral("opus"));

    p.onTurnEnded();
    QCOMPARE(p.sessionSelection()->model, QStringLiteral("opus"));
    QCOMPARE(p.sessionSelection()->effort, QStringLiteral("high"));

    p.onSelectionReported(kOpusHigh);
    QVERIFY(!p.sessionSelection().has_value());
}

void TestAcpPrewalk::editBeforeTheRestoreLandsKeepsTheSessionSelection()
{
    // A wake-up turn starts before the agent answered the restore.
    AcpPrewalk p = runningTurn();
    p.onToolCall(edit(), kOpusHigh);
    p.onTurnEnded();

    p.onTurnStarted();
    QCOMPARE(p.onToolCall(edit(), kSonnetHigh), QStringLiteral("sonnet"));
    QCOMPARE(p.onTurnEnded()->model, QStringLiteral("opus"));
}

void TestAcpPrewalk::resetForgetsEverything()
{
    AcpPrewalk p = runningTurn();
    p.onToolCall(edit(), kOpusHigh);
    p.reset();

    QVERIFY(!p.isSwitched());
    QVERIFY(!p.sessionSelection().has_value());
    QVERIFY(!p.onTurnEnded().has_value());
    QCOMPARE(p.target(), QStringLiteral("sonnet"));
    // No turn is running after a reset.
    QCOMPARE(p.onToolCall(edit(), kOpusHigh), QString());
}

QTEST_APPLESS_MAIN(TestAcpPrewalk)
#include "test_acp_prewalk.moc"
