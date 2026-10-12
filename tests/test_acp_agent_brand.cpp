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

#include "AcpAgentBrand.h"
#include "AcpAgentDefinition.h"


class TestAcpAgentBrand : public QObject
{
    Q_OBJECT

private slots:
    void detect_data();
    void detect();
    void explicitBrandKey_winsOverDetection();
    void customIconPath_hasNoBrand();
    void brandKeys_areKnown();
};

namespace {
AcpAgentDefinition makeDef(const QString &id, const QString &name,
                           const QString &command, const QStringList &args)
{
    AcpAgentDefinition def;
    def.id = id;
    def.name = name;
    def.command = command;
    def.args = args;
    return def;
}
}

void TestAcpAgentBrand::detect_data()
{
    QTest::addColumn<QString>("id");
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("command");
    QTest::addColumn<QStringList>("args");
    QTest::addColumn<QString>("expected");

    const QString uuidId = QStringLiteral("user:0f3c9a2e-7b1d-4e55-a0cd-1234567890ab");

    QTest::newRow("builtin claude") << QStringLiteral("builtin:claude-code")
        << QStringLiteral("Claude Code") << QStringLiteral("npx")
        << QStringList{QStringLiteral("-y"), QStringLiteral("@agentclientprotocol/claude-agent-acp@latest")}
        << QStringLiteral("claude-code");
    QTest::newRow("claude by args only") << uuidId << QStringLiteral("Work agent")
        << QStringLiteral("npx")
        << QStringList{QStringLiteral("@agentclientprotocol/claude-agent-acp")}
        << QStringLiteral("claude-code");
    QTest::newRow("builtin codex") << QStringLiteral("builtin:codex")
        << QStringLiteral("Codex") << QStringLiteral("npx")
        << QStringList{QStringLiteral("-y"), QStringLiteral("@zed-industries/codex-acp")}
        << QStringLiteral("codex");
    QTest::newRow("opencode") << uuidId << QStringLiteral("OpenCode")
        << QStringLiteral("opencode") << QStringList{QStringLiteral("acp")}
        << QStringLiteral("opencode");
    QTest::newRow("open-code spaced") << uuidId << QStringLiteral("Open Code")
        << QStringLiteral("npx") << QStringList{}
        << QStringLiteral("opencode");
    QTest::newRow("omp command") << uuidId << QStringLiteral("My agent")
        << QStringLiteral("omp") << QStringList{QStringLiteral("acp")}
        << QStringLiteral("oh-my-pi");
    QTest::newRow("oh my pi name") << uuidId << QStringLiteral("Oh My Pi")
        << QStringLiteral("npx") << QStringList{}
        << QStringLiteral("oh-my-pi");
    QTest::newRow("pi-acp") << uuidId << QStringLiteral("PI")
        << QStringLiteral("npx") << QStringList{QStringLiteral("-y"), QStringLiteral("pi-acp")}
        << QStringLiteral("pi");
    QTest::newRow("pi in windows path") << uuidId << QStringLiteral("Local")
        << QStringLiteral("C:\\tools\\pi.exe") << QStringList{}
        << QStringLiteral("pi");
    QTest::newRow("pipeline is not pi") << uuidId << QStringLiteral("Pipeline")
        << QStringLiteral("pipx") << QStringList{QStringLiteral("run"), QStringLiteral("spinner")}
        << QString();
    QTest::newRow("compose is not omp") << uuidId << QStringLiteral("Compose")
        << QStringLiteral("docker") << QStringList{QStringLiteral("compose")}
        << QString();
    QTest::newRow("unknown") << uuidId << QStringLiteral("Gemini")
        << QStringLiteral("gemini") << QStringList{QStringLiteral("--experimental-acp")}
        << QString();
}

void TestAcpAgentBrand::detect()
{
    QFETCH(QString, id);
    QFETCH(QString, name);
    QFETCH(QString, command);
    QFETCH(QStringList, args);
    QFETCH(QString, expected);

    const AcpAgentDefinition def = makeDef(id, name, command, args);
    QCOMPARE(detectAcpAgentBrand(def), expected);
    QCOMPARE(acpAgentBrandKey(def), expected);
}

void TestAcpAgentBrand::explicitBrandKey_winsOverDetection()
{
    AcpAgentDefinition def = makeDef(QStringLiteral("builtin:claude-code"),
        QStringLiteral("Claude Code"), QStringLiteral("npx"), {});
    def.icon = QStringLiteral("codex");
    QCOMPARE(acpAgentBrandKey(def), QStringLiteral("codex"));
}

void TestAcpAgentBrand::customIconPath_hasNoBrand()
{
    AcpAgentDefinition def = makeDef(QStringLiteral("builtin:claude-code"),
        QStringLiteral("Claude Code"), QStringLiteral("npx"), {});
    def.icon = QStringLiteral("C:/icons/mine.png");
    QVERIFY(acpAgentBrandKey(def).isEmpty());
}

void TestAcpAgentBrand::brandKeys_areKnown()
{
    const QStringList expected{
        QStringLiteral("claude-code"), QStringLiteral("codex"), QStringLiteral("opencode"),
        QStringLiteral("oh-my-pi"), QStringLiteral("pi"),
    };
    QStringList keys;
    for (const AcpAgentBrandInfo &info : acpAgentBrands()) {
        keys.append(QLatin1String(info.key));
        QVERIFY(isAcpAgentBrandKey(QLatin1String(info.key)));
    }
    QCOMPARE(keys, expected);
    QVERIFY(!isAcpAgentBrandKey(QString()));
    QVERIFY(!isAcpAgentBrandKey(QStringLiteral("gemini")));
}

QTEST_MAIN(TestAcpAgentBrand)

#include "test_acp_agent_brand.moc"
