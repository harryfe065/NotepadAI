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

#include <QtTest>
#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "AcpAgentDefinition.h"
#include "AcpAgentRegistry.h"
#include "ApplicationSettings.h"


class TestAcpAgentRegistry : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void firstLaunch_seedsBuiltinAgents();
    void userAddRoundTrip_persistsAcrossInstances();
    void legacyUserAgentsArray_keepsFactorySeeds();
    void removeSeededAgent_persistsAcrossReload();
    void updateSeededAgent_persistsAcrossReload();
    void duplicateId_isRefused();
    void defaultAgentId_fallsBackToBuiltin();
    void defaultAgentId_usesFirstRemainingAfterDefaultRemoved();
    void autoApprovePolicy_defaultsToManual();
    void setAutoApprovePolicy_persistsAndEmits();
    void moveAgent_reordersPersistsAndEmits();
    void moveAgent_rejectsNoOpAndInvalid();
    void cloneAgent_insertsUserCopyAfterSource();
    void cloneAgent_namesStayUnique();
    void cloneAgent_unknownId_returnsEmpty();

private:
    QTemporaryDir tempDir;
};

void TestAcpAgentRegistry::initTestCase()
{
    QVERIFY(tempDir.isValid());
    QCoreApplication::setOrganizationName("NotepadNextTest");
    QCoreApplication::setApplicationName("NotepadNextTest");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());
}

void TestAcpAgentRegistry::init()
{
    ApplicationSettings s;
    s.clear();
    s.sync();
}

void TestAcpAgentRegistry::firstLaunch_seedsBuiltinAgents()
{
    ApplicationSettings s;
    AcpAgentRegistry registry(&s);

    const auto agents = registry.agents();
    QCOMPARE(agents.size(), 2);
    QCOMPARE(agents.first().id, AcpAgentRegistry::builtinClaudeCodeId());
    QVERIFY(agents.first().builtin);
    QCOMPARE(agents.first().command, QStringLiteral("npx"));
    QCOMPARE(agents.at(1).id, AcpAgentRegistry::builtinCodexId());
    QVERIFY(agents.at(1).builtin);
    QCOMPARE(agents.at(1).command, QStringLiteral("npx"));
    const QStringList codexArgs{
        QStringLiteral("-y"),
        QStringLiteral("@zed-industries/codex-acp"),
    };
    QCOMPARE(agents.at(1).args, codexArgs);
}

void TestAcpAgentRegistry::userAddRoundTrip_persistsAcrossInstances()
{
    ApplicationSettings s;

    {
        AcpAgentRegistry registry(&s);
        AcpAgentDefinition def;
        def.id = QStringLiteral("user:foo");
        def.name = QStringLiteral("Foo");
        def.command = QStringLiteral("foo");
        def.builtin = false;
        QVERIFY(registry.addAgent(def));
    }
    s.sync();

    AcpAgentRegistry reloaded(&s);
    const auto agents = reloaded.agents();
    QCOMPARE(agents.size(), 3);
    QVERIFY(reloaded.contains(QStringLiteral("user:foo")));
    QVERIFY(reloaded.contains(AcpAgentRegistry::builtinClaudeCodeId()));
    QVERIFY(reloaded.contains(AcpAgentRegistry::builtinCodexId()));
    QCOMPARE(reloaded.agent(QStringLiteral("user:foo")).name, QStringLiteral("Foo"));
}

void TestAcpAgentRegistry::legacyUserAgentsArray_keepsFactorySeeds()
{
    ApplicationSettings s;
    s.setAiAgentsJson(QStringLiteral(
        "[{\"id\":\"user:foo\",\"name\":\"Foo\",\"command\":\"foo\","
        "\"args\":[],\"argsTokenized\":true,\"env\":{},\"icon\":\"\",\"builtin\":false}]"));
    s.sync();

    AcpAgentRegistry registry(&s);
    QCOMPARE(registry.agents().size(), 3);
    QVERIFY(registry.contains(AcpAgentRegistry::builtinClaudeCodeId()));
    QVERIFY(registry.contains(AcpAgentRegistry::builtinCodexId()));
    QVERIFY(registry.contains(QStringLiteral("user:foo")));
}

void TestAcpAgentRegistry::removeSeededAgent_persistsAcrossReload()
{
    ApplicationSettings s;
    {
        AcpAgentRegistry registry(&s);
        QVERIFY(registry.removeAgent(AcpAgentRegistry::builtinClaudeCodeId()));
        QVERIFY(!registry.contains(AcpAgentRegistry::builtinClaudeCodeId()));
        QVERIFY(registry.contains(AcpAgentRegistry::builtinCodexId()));
        QCOMPARE(registry.agents().size(), 1);
    }
    s.sync();

    AcpAgentRegistry reloaded(&s);
    QVERIFY(!reloaded.contains(AcpAgentRegistry::builtinClaudeCodeId()));
    QVERIFY(reloaded.contains(AcpAgentRegistry::builtinCodexId()));
    QCOMPARE(reloaded.agents().size(), 1);
}

void TestAcpAgentRegistry::updateSeededAgent_persistsAcrossReload()
{
    ApplicationSettings s;
    {
        AcpAgentRegistry registry(&s);
        AcpAgentDefinition def = registry.agent(AcpAgentRegistry::builtinClaudeCodeId());
        def.command = QStringLiteral("claude");
        def.args = QStringList{QStringLiteral("acp")};
        QVERIFY(registry.updateAgent(def));
        QCOMPARE(registry.agent(AcpAgentRegistry::builtinClaudeCodeId()).command,
                 QStringLiteral("claude"));
    }
    s.sync();

    AcpAgentRegistry reloaded(&s);
    const AcpAgentDefinition got = reloaded.agent(AcpAgentRegistry::builtinClaudeCodeId());
    QCOMPARE(got.command, QStringLiteral("claude"));
    QCOMPARE(got.args, QStringList{QStringLiteral("acp")});
    QCOMPARE(got.name, QStringLiteral("Claude Code"));
}

void TestAcpAgentRegistry::duplicateId_isRefused()
{
    ApplicationSettings s;
    AcpAgentRegistry registry(&s);

    AcpAgentDefinition def;
    def.id = QStringLiteral("user:foo");
    def.name = QStringLiteral("Foo");
    def.command = QStringLiteral("foo");
    QVERIFY(registry.addAgent(def));

    AcpAgentDefinition dup;
    dup.id = QStringLiteral("user:foo");
    dup.name = QStringLiteral("Foo2");
    dup.command = QStringLiteral("bar");
    QVERIFY(!registry.addAgent(dup));
    QCOMPARE(registry.agents().size(), 3);

    // Built-in id should also be refused.
    AcpAgentDefinition collidesWithBuiltin;
    collidesWithBuiltin.id = AcpAgentRegistry::builtinCodexId();
    collidesWithBuiltin.name = QStringLiteral("Hijack");
    collidesWithBuiltin.command = QStringLiteral("evil");
    QVERIFY(!registry.addAgent(collidesWithBuiltin));
}

void TestAcpAgentRegistry::defaultAgentId_fallsBackToBuiltin()
{
    ApplicationSettings s;
    AcpAgentRegistry registry(&s);

    QCOMPARE(registry.defaultAgentId(), AcpAgentRegistry::builtinClaudeCodeId());
}

void TestAcpAgentRegistry::defaultAgentId_usesFirstRemainingAfterDefaultRemoved()
{
    ApplicationSettings s;
    AcpAgentRegistry registry(&s);
    QVERIFY(registry.removeAgent(AcpAgentRegistry::builtinClaudeCodeId()));
    QCOMPARE(registry.defaultAgentId(), AcpAgentRegistry::builtinCodexId());

    QVERIFY(registry.removeAgent(AcpAgentRegistry::builtinCodexId()));
    QVERIFY(registry.defaultAgentId().isEmpty());
}

void TestAcpAgentRegistry::autoApprovePolicy_defaultsToManual()
{
    ApplicationSettings s;
    AcpAgentRegistry registry(&s);

    QCOMPARE(registry.autoApprovePolicy(), QStringLiteral("manual"));
}

void TestAcpAgentRegistry::setAutoApprovePolicy_persistsAndEmits()
{
    ApplicationSettings s;
    AcpAgentRegistry registry(&s);

    QSignalSpy spy(&registry, &AcpAgentRegistry::autoApprovePolicyChanged);
    QVERIFY(spy.isValid());

    registry.setAutoApprovePolicy(QStringLiteral("allowAll"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("allowAll"));
    QCOMPARE(registry.autoApprovePolicy(), QStringLiteral("allowAll"));

    s.sync();
    AcpAgentRegistry reloaded(&s);
    QCOMPARE(reloaded.autoApprovePolicy(), QStringLiteral("allowAll"));
}

void TestAcpAgentRegistry::moveAgent_reordersPersistsAndEmits()
{
    ApplicationSettings s;
    {
        AcpAgentRegistry registry(&s);
        AcpAgentDefinition def;
        def.id = QStringLiteral("user:foo");
        def.name = QStringLiteral("Foo");
        def.command = QStringLiteral("foo");
        QVERIFY(registry.addAgent(def));

        QSignalSpy spy(&registry, &AcpAgentRegistry::changed);
        QVERIFY(registry.moveAgent(QStringLiteral("user:foo"), 0));
        QCOMPARE(spy.count(), 1);

        const auto agents = registry.agents();
        QCOMPARE(agents.size(), 3);
        QCOMPARE(agents.at(0).id, QStringLiteral("user:foo"));
        QCOMPARE(agents.at(1).id, AcpAgentRegistry::builtinClaudeCodeId());
        QCOMPARE(agents.at(2).id, AcpAgentRegistry::builtinCodexId());

        QVERIFY(registry.moveAgent(AcpAgentRegistry::builtinClaudeCodeId(), 2));
        QCOMPARE(registry.agents().at(2).id, AcpAgentRegistry::builtinClaudeCodeId());
        QCOMPARE(registry.agents().at(1).id, AcpAgentRegistry::builtinCodexId());
    }
    s.sync();

    AcpAgentRegistry reloaded(&s);
    const auto agents = reloaded.agents();
    QCOMPARE(agents.size(), 3);
    QCOMPARE(agents.at(0).id, QStringLiteral("user:foo"));
    QCOMPARE(agents.at(1).id, AcpAgentRegistry::builtinCodexId());
    QCOMPARE(agents.at(2).id, AcpAgentRegistry::builtinClaudeCodeId());
}

void TestAcpAgentRegistry::moveAgent_rejectsNoOpAndInvalid()
{
    ApplicationSettings s;
    AcpAgentRegistry registry(&s);
    QSignalSpy spy(&registry, &AcpAgentRegistry::changed);

    QVERIFY(!registry.moveAgent(AcpAgentRegistry::builtinClaudeCodeId(), 0));
    QVERIFY(!registry.moveAgent(QStringLiteral("user:missing"), 0));
    QVERIFY(!registry.moveAgent(AcpAgentRegistry::builtinClaudeCodeId(), -1));
    QVERIFY(!registry.moveAgent(AcpAgentRegistry::builtinClaudeCodeId(), 2));
    QCOMPARE(spy.count(), 0);
    QCOMPARE(registry.agents().at(0).id, AcpAgentRegistry::builtinClaudeCodeId());
}

void TestAcpAgentRegistry::cloneAgent_insertsUserCopyAfterSource()
{
    ApplicationSettings s;
    QString cloneId;
    {
        AcpAgentRegistry registry(&s);
        AcpAgentDefinition src = registry.agent(AcpAgentRegistry::builtinClaudeCodeId());
        src.env.insert(QStringLiteral("KEY"), QStringLiteral("value"));
        src.icon = QStringLiteral("claude-code");
        QVERIFY(registry.updateAgent(src));

        QSignalSpy spy(&registry, &AcpAgentRegistry::changed);
        cloneId = registry.cloneAgent(AcpAgentRegistry::builtinClaudeCodeId());
        QCOMPARE(spy.count(), 1);
        QVERIFY(cloneId.startsWith(QStringLiteral("user:")));
        QVERIFY(cloneId != AcpAgentRegistry::builtinClaudeCodeId());

        const auto agents = registry.agents();
        QCOMPARE(agents.size(), 3);
        QCOMPARE(agents.at(0).id, AcpAgentRegistry::builtinClaudeCodeId());
        QCOMPARE(agents.at(1).id, cloneId);
        QCOMPARE(agents.at(2).id, AcpAgentRegistry::builtinCodexId());

        const AcpAgentDefinition copy = agents.at(1);
        QCOMPARE(copy.name, QStringLiteral("Claude Code (copy)"));
        QCOMPARE(copy.command, src.command);
        QCOMPARE(copy.args, src.args);
        QCOMPARE(copy.env, src.env);
        QCOMPARE(copy.icon, src.icon);
        QVERIFY(!copy.builtin);
    }
    s.sync();

    AcpAgentRegistry reloaded(&s);
    QCOMPARE(reloaded.agents().at(1).id, cloneId);
    QCOMPARE(reloaded.agent(cloneId).name, QStringLiteral("Claude Code (copy)"));
}

void TestAcpAgentRegistry::cloneAgent_namesStayUnique()
{
    ApplicationSettings s;
    AcpAgentRegistry registry(&s);

    const QString first = registry.cloneAgent(AcpAgentRegistry::builtinCodexId());
    const QString second = registry.cloneAgent(AcpAgentRegistry::builtinCodexId());
    // Cloning a copy strips its "(copy N)" suffix instead of stacking another.
    const QString third = registry.cloneAgent(first);

    QCOMPARE(registry.agent(first).name, QStringLiteral("Codex (copy)"));
    QCOMPARE(registry.agent(second).name, QStringLiteral("Codex (copy 2)"));
    QCOMPARE(registry.agent(third).name, QStringLiteral("Codex (copy 3)"));
}

void TestAcpAgentRegistry::cloneAgent_unknownId_returnsEmpty()
{
    ApplicationSettings s;
    AcpAgentRegistry registry(&s);
    QSignalSpy spy(&registry, &AcpAgentRegistry::changed);

    QVERIFY(registry.cloneAgent(QStringLiteral("user:missing")).isEmpty());
    QCOMPARE(spy.count(), 0);
    QCOMPARE(registry.agents().size(), 2);
}

QTEST_MAIN(TestAcpAgentRegistry)

#include "test_acp_agent_registry.moc"
