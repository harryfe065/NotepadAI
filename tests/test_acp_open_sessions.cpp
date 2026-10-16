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

#include "AcpOpenSessions.h"

#include <QtTest>

using AcpOpenSessions::Group;
using AcpOpenSessions::Session;

class TestAcpOpenSessions : public QObject
{
    Q_OBJECT

private slots:
    void roundTripKeepsOrderAndCurrent();
    void garbageAndEmptyGroupsAreDropped();
    void currentIsClampedIntoRange();
};

void TestAcpOpenSessions::roundTripKeepsOrderAndCurrent()
{
    Group local;
    local.cwd = QStringLiteral("D:/projects/Cpp/notepad-ade");
    local.current = 1;
    local.sessions = {{QStringLiteral("s1"), QStringLiteral("cc-a"), QStringLiteral("CC nullmastermind")},
                      {QStringLiteral("s2"), QStringLiteral("cc-b"), QStringLiteral("CC 0x317374")}};
    Group remote;
    remote.cwd = QStringLiteral("/home/me/app");
    remote.sshProfileId = QStringLiteral("prof-1");
    remote.sessions = {{QStringLiteral("s3"), QStringLiteral("codex"), QStringLiteral("Codex")}};

    const QVector<Group> back = AcpOpenSessions::fromJson(AcpOpenSessions::toJson({local, remote}));
    QCOMPARE(back.size(), 2);
    QCOMPARE(back.at(0).cwd, local.cwd);
    QCOMPARE(back.at(0).current, 1);
    QCOMPARE(back.at(0).sessions.size(), 2);
    QCOMPARE(back.at(0).sessions.at(1).id, QStringLiteral("s2"));
    QCOMPARE(back.at(0).sessions.at(1).agentId, QStringLiteral("cc-b"));
    QCOMPARE(back.at(0).sessions.at(1).agentName, QStringLiteral("CC 0x317374"));
    QCOMPARE(back.at(1).sshProfileId, QStringLiteral("prof-1"));
    QCOMPARE(back.at(1).sessions.at(0).id, QStringLiteral("s3"));
}

void TestAcpOpenSessions::garbageAndEmptyGroupsAreDropped()
{
    QVERIFY(AcpOpenSessions::fromJson("not json").isEmpty());
    QVERIFY(AcpOpenSessions::fromJson("{}").isEmpty());
    const QByteArray json = R"([
        {"cwd": "", "sessions": [{"id": "x", "agentId": "a"}]},
        {"cwd": "D:/p", "sessions": []},
        {"cwd": "D:/q", "sessions": [{"id": "", "agentId": "a"}, {"id": "keep", "agentId": "a"}]}
    ])";
    const QVector<Group> back = AcpOpenSessions::fromJson(json);
    QCOMPARE(back.size(), 1);
    QCOMPARE(back.at(0).cwd, QStringLiteral("D:/q"));
    QCOMPARE(back.at(0).sessions.size(), 1);
    QCOMPARE(back.at(0).sessions.at(0).id, QStringLiteral("keep"));
}

void TestAcpOpenSessions::currentIsClampedIntoRange()
{
    const QVector<Group> back = AcpOpenSessions::fromJson(
        R"([{"cwd": "D:/p", "current": 7, "sessions": [{"id": "a"}, {"id": "b"}]}])");
    QCOMPARE(back.size(), 1);
    QCOMPARE(back.at(0).current, 1);
}

QTEST_GUILESS_MAIN(TestAcpOpenSessions)
#include "test_acp_open_sessions.moc"
