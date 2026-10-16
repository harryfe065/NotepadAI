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

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "ai/CommitMessageTool.h"

using ai::CommitMessageTool::parseArguments;
using ai::CommitMessageTool::Problem;

class TestCommitMessageTool : public QObject
{
    Q_OBJECT

private slots:
    void tool_schemaRequiresTypeAndSubjectWithTypeEnum();
    void systemPrompt_namesTheTool();
    void parse_typeScopeSubjectBody_joinsWithBlankLine();
    void parse_noScope_omitsParentheses();
    void parse_noBody_omitsBlankLine();
    void parse_emptyScopeAndBody_treatedAsAbsent();
    void parse_trimsWhitespaceOnly();
    void parse_headerOver50_stillAccepted();
    void parse_notJson_isInvalidJson();
    void parse_truncatedJson_isInvalidJson();
    void parse_unknownType_isInvalidType();
    void parse_typeCaseIsNotRepaired();
    void parse_scopeWithForbiddenChars_isInvalidScope();
    void parse_scopeNotString_isInvalidScope();
    void parse_emptySubject_isInvalidSubject();
    void parse_multiLineSubject_isInvalidSubject();
    void parse_bodyNotString_isInvalidBody();
    void parse_bodyKeptAsIs_noRewrapping();
};

static QByteArray args(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

void TestCommitMessageTool::tool_schemaRequiresTypeAndSubjectWithTypeEnum()
{
    const auto t = ai::CommitMessageTool::tool();
    QCOMPARE(t.name, QStringLiteral("submit_commit_message"));
    QCOMPARE(t.inputSchema.value(QLatin1String("required")).toArray(),
             (QJsonArray{QLatin1String("type"), QLatin1String("subject")}));
    const QJsonObject props = t.inputSchema.value(QLatin1String("properties")).toObject();
    QCOMPARE(props.value(QLatin1String("type")).toObject().value(QLatin1String("enum")).toArray().size(), 11);
    QVERIFY(props.contains(QLatin1String("scope")));
    QVERIFY(props.contains(QLatin1String("body")));
}

void TestCommitMessageTool::systemPrompt_namesTheTool()
{
    QVERIFY(ai::CommitMessageTool::systemPrompt().contains(QLatin1String("submit_commit_message")));
}

void TestCommitMessageTool::parse_typeScopeSubjectBody_joinsWithBlankLine()
{
    const auto r = parseArguments(args({{"type", "feat"}, {"scope", "ai"},
                                        {"subject", "add tool call"}, {"body", "Because."}}));
    QVERIFY(r.ok());
    QCOMPARE(r.message, QStringLiteral("feat(ai): add tool call\n\nBecause."));
}

void TestCommitMessageTool::parse_noScope_omitsParentheses()
{
    const auto r = parseArguments(args({{"type", "fix"}, {"subject", "stop crash"}}));
    QVERIFY(r.ok());
    QCOMPARE(r.message, QStringLiteral("fix: stop crash"));
}

void TestCommitMessageTool::parse_noBody_omitsBlankLine()
{
    const auto r = parseArguments(args({{"type", "chore"}, {"scope", "x"}, {"subject", "bump"}}));
    QVERIFY(r.ok());
    QVERIFY(!r.message.contains(QLatin1Char('\n')));
}

void TestCommitMessageTool::parse_emptyScopeAndBody_treatedAsAbsent()
{
    const auto r = parseArguments(args({{"type", "docs"}, {"scope", "  "},
                                        {"subject", "tweak"}, {"body", "\n "}}));
    QVERIFY(r.ok());
    QCOMPARE(r.message, QStringLiteral("docs: tweak"));
}

void TestCommitMessageTool::parse_trimsWhitespaceOnly()
{
    const auto r = parseArguments(args({{"type", " perf "}, {"scope", " db "},
                                        {"subject", "  faster  "}, {"body", "\n\nWhy.\n"}}));
    QVERIFY(r.ok());
    QCOMPARE(r.message, QStringLiteral("perf(db): faster\n\nWhy."));
}

void TestCommitMessageTool::parse_headerOver50_stillAccepted()
{
    const QString subject(70, QLatin1Char('a'));
    const auto r = parseArguments(args({{"type", "feat"}, {"subject", subject}}));
    QVERIFY(r.ok());
    QVERIFY(r.message.size() > 50);
}

void TestCommitMessageTool::parse_notJson_isInvalidJson()
{
    const auto r = parseArguments("hello");
    QCOMPARE(r.problem, Problem::InvalidJson);
    QVERIFY(!r.error.isEmpty());
    QVERIFY(r.message.isEmpty());
}

void TestCommitMessageTool::parse_truncatedJson_isInvalidJson()
{
    QCOMPARE(parseArguments("{\"type\":\"feat\",\"subject\":\"abc").problem, Problem::InvalidJson);
    QCOMPARE(parseArguments("").problem, Problem::InvalidJson);
}

void TestCommitMessageTool::parse_unknownType_isInvalidType()
{
    QCOMPARE(parseArguments(args({{"type", "wip"}, {"subject", "x"}})).problem, Problem::InvalidType);
    QCOMPARE(parseArguments(args({{"subject", "x"}})).problem, Problem::InvalidType);
}

void TestCommitMessageTool::parse_typeCaseIsNotRepaired()
{
    QCOMPARE(parseArguments(args({{"type", "Feat"}, {"subject", "x"}})).problem, Problem::InvalidType);
}

void TestCommitMessageTool::parse_scopeWithForbiddenChars_isInvalidScope()
{
    for (const char *scope : {"a(b", "a)b", "a:b", "a\nb"})
        QCOMPARE(parseArguments(args({{"type", "fix"}, {"scope", scope}, {"subject", "x"}})).problem,
                 Problem::InvalidScope);
}

void TestCommitMessageTool::parse_scopeNotString_isInvalidScope()
{
    QCOMPARE(parseArguments(args({{"type", "fix"}, {"scope", 3}, {"subject", "x"}})).problem,
             Problem::InvalidScope);
}

void TestCommitMessageTool::parse_emptySubject_isInvalidSubject()
{
    QCOMPARE(parseArguments(args({{"type", "fix"}, {"subject", "  "}})).problem, Problem::InvalidSubject);
    QCOMPARE(parseArguments(args({{"type", "fix"}})).problem, Problem::InvalidSubject);
}

void TestCommitMessageTool::parse_multiLineSubject_isInvalidSubject()
{
    QCOMPARE(parseArguments(args({{"type", "fix"}, {"subject", "a\nb"}})).problem, Problem::InvalidSubject);
}

void TestCommitMessageTool::parse_bodyNotString_isInvalidBody()
{
    QCOMPARE(parseArguments(args({{"type", "fix"}, {"subject", "x"}, {"body", QJsonArray{"a"}}})).problem,
             Problem::InvalidBody);
}

void TestCommitMessageTool::parse_bodyKeptAsIs_noRewrapping()
{
    const QString body = QStringLiteral("- one very long bullet ") + QString(100, QLatin1Char('x'))
        + QStringLiteral("\n- two");
    const auto r = parseArguments(args({{"type", "fix"}, {"subject", "x"}, {"body", body}}));
    QVERIFY(r.ok());
    QVERIFY(r.message.endsWith(body));
}

QTEST_APPLESS_MAIN(TestCommitMessageTool)

#include "test_commit_message_tool.moc"
