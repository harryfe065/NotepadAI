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

#include "ai/SsePartialParser.h"


class TestSsePartialParser : public QObject
{
    Q_OBJECT

private slots:
    void anthropicContentBlockDelta_emitsToken();
    void anthropicMessageStop_emitsDone();
    void anthropicPing_emitsNothing();
    void anthropicErrorEvent_emitsError();
    void anthropicThinkingDelta_emitsNoToken();
    void anthropicMessageStart_emitsNoToken();
    void anthropicStream_startDeltaStop_emitsTokensThenDone();
    void anthropicNonStream_contentArrayText_emitsTokenAndDone();
    void anthropicNonStream_emptyContent_emitsDoneOnly();
    void anthropicNonStream_multipleTextBlocks_concatenatesIgnoringToolUse();
    void anthropicNonStream_missingStopReason_emitsTokenWithoutDone();
    void anthropicSse_partialChunkThenRest_emitsToken();
    void anthropicToolUse_startThenInputJsonDeltas();
    void anthropicMessageDelta_emitsStopReason();
    void openaiToolCalls_nameThenArgumentFragments();
    void openaiFinishReason_emitsStopReasonBeforeDone();
    void openaiToolCallWithoutIndex_usesArrayPosition();
};

namespace {

using Ev = ai::SsePartialParser::Event;
using Kind = ai::SsePartialParser::EventKind;

QList<Ev> feedAll(const QByteArray &bytes)
{
    ai::SsePartialParser parser;
    QList<Ev> out;
    parser.feed(bytes, [&](const Ev &ev) { out.append(ev); });
    return out;
}

} // namespace

void TestSsePartialParser::anthropicContentBlockDelta_emitsToken()
{
    ai::SsePartialParser parser;
    QStringList tokens;
    const QByteArray chunk =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n"
        "\n";
    parser.feed(chunk, [&](const ai::SsePartialParser::Event &ev) {
        if (ev.kind == ai::SsePartialParser::EventKind::Token)
            tokens.append(ev.text);
    });
    QCOMPARE(tokens, QStringList{QStringLiteral("Hello")});
}

void TestSsePartialParser::anthropicMessageStop_emitsDone()
{
    ai::SsePartialParser parser;
    int doneCount = 0;
    const QByteArray chunk =
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n"
        "\n";
    parser.feed(chunk, [&](const ai::SsePartialParser::Event &ev) {
        if (ev.kind == ai::SsePartialParser::EventKind::Done)
            ++doneCount;
    });
    QCOMPARE(doneCount, 1);
}

void TestSsePartialParser::anthropicPing_emitsNothing()
{
    ai::SsePartialParser parser;
    int events = 0;
    const QByteArray chunk = "event: ping\ndata: {}\n\n";
    parser.feed(chunk, [&](const ai::SsePartialParser::Event &) { ++events; });
    QCOMPARE(events, 0);
}

void TestSsePartialParser::anthropicErrorEvent_emitsError()
{
    ai::SsePartialParser parser;
    QString err;
    const QByteArray chunk =
        "event: error\n"
        "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
        "\"message\":\"Overloaded\"}}\n"
        "\n";
    parser.feed(chunk, [&](const ai::SsePartialParser::Event &ev) {
        if (ev.kind == ai::SsePartialParser::EventKind::Error)
            err = ev.text;
    });
    QCOMPARE(err, QStringLiteral("Overloaded"));
}

void TestSsePartialParser::anthropicThinkingDelta_emitsNoToken()
{
    ai::SsePartialParser parser;
    int tokens = 0;
    const QByteArray chunk =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":"
        "{\"type\":\"thinking_delta\",\"thinking\":\"hmm\"}}\n"
        "\n";
    parser.feed(chunk, [&](const ai::SsePartialParser::Event &ev) {
        if (ev.kind == ai::SsePartialParser::EventKind::Token)
            ++tokens;
    });
    QCOMPARE(tokens, 0);
}

void TestSsePartialParser::anthropicMessageStart_emitsNoToken()
{
    ai::SsePartialParser parser;
    int events = 0;
    const QByteArray chunk =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\","
        "\"type\":\"message\",\"role\":\"assistant\",\"content\":[],"
        "\"model\":\"claude-opus-5\",\"stop_reason\":null}}\n"
        "\n";
    parser.feed(chunk, [&](const ai::SsePartialParser::Event &) { ++events; });
    QCOMPARE(events, 0);
}

void TestSsePartialParser::anthropicStream_startDeltaStop_emitsTokensThenDone()
{
    ai::SsePartialParser parser;
    QStringList tokens;
    int doneCount = 0;
    const QByteArray chunk =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"content\":[]}}\n"
        "\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":"
        "{\"type\":\"text_delta\",\"text\":\"Hel\"}}\n"
        "\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":"
        "{\"type\":\"text_delta\",\"text\":\"lo\"}}\n"
        "\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n"
        "\n";
    parser.feed(chunk, [&](const ai::SsePartialParser::Event &ev) {
        if (ev.kind == ai::SsePartialParser::EventKind::Token)
            tokens.append(ev.text);
        else if (ev.kind == ai::SsePartialParser::EventKind::Done)
            ++doneCount;
    });
    QCOMPARE(tokens, (QStringList{QStringLiteral("Hel"), QStringLiteral("lo")}));
    QCOMPARE(doneCount, 1);
}

void TestSsePartialParser::anthropicNonStream_contentArrayText_emitsTokenAndDone()
{
    ai::SsePartialParser parser;
    QStringList tokens;
    int doneCount = 0;
    const QByteArray chunk =
        "data: {\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"text\",\"text\":\"Hello\"}],"
        "\"stop_reason\":\"end_turn\"}\n"
        "\n";
    parser.feed(chunk, [&](const ai::SsePartialParser::Event &ev) {
        if (ev.kind == ai::SsePartialParser::EventKind::Token)
            tokens.append(ev.text);
        else if (ev.kind == ai::SsePartialParser::EventKind::Done)
            ++doneCount;
    });
    QCOMPARE(tokens, QStringList{QStringLiteral("Hello")});
    QCOMPARE(doneCount, 1);
}

void TestSsePartialParser::anthropicNonStream_emptyContent_emitsDoneOnly()
{
    ai::SsePartialParser parser;
    int tokens = 0;
    int doneCount = 0;
    const QByteArray chunk =
        "data: {\"type\":\"message\",\"content\":[],\"stop_reason\":\"end_turn\"}\n\n";
    parser.feed(chunk, [&](const ai::SsePartialParser::Event &ev) {
        if (ev.kind == ai::SsePartialParser::EventKind::Token) ++tokens;
        else if (ev.kind == ai::SsePartialParser::EventKind::Done) ++doneCount;
    });
    QCOMPARE(tokens, 0);
    QCOMPARE(doneCount, 1);
}

void TestSsePartialParser::anthropicNonStream_multipleTextBlocks_concatenatesIgnoringToolUse()
{
    ai::SsePartialParser parser;
    QStringList tokens;
    const QByteArray chunk =
        "data: {\"type\":\"message\",\"content\":["
        "{\"type\":\"text\",\"text\":\"A\"},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"f\",\"input\":{}},"
        "{\"type\":\"text\",\"text\":\"B\"}],"
        "\"stop_reason\":\"tool_use\"}\n\n";
    parser.feed(chunk, [&](const ai::SsePartialParser::Event &ev) {
        if (ev.kind == ai::SsePartialParser::EventKind::Token)
            tokens.append(ev.text);
    });
    QCOMPARE(tokens, QStringList{QStringLiteral("AB")});
}

void TestSsePartialParser::anthropicNonStream_missingStopReason_emitsTokenWithoutDone()
{
    ai::SsePartialParser parser;
    QStringList tokens;
    int doneCount = 0;
    const QByteArray chunk =
        "data: {\"type\":\"message\",\"content\":[{\"type\":\"text\",\"text\":\"Hi\"}]}\n\n";
    parser.feed(chunk, [&](const ai::SsePartialParser::Event &ev) {
        if (ev.kind == ai::SsePartialParser::EventKind::Token)
            tokens.append(ev.text);
        else if (ev.kind == ai::SsePartialParser::EventKind::Done)
            ++doneCount;
    });
    QCOMPARE(tokens, QStringList{QStringLiteral("Hi")});
    QCOMPARE(doneCount, 0);
}

void TestSsePartialParser::anthropicSse_partialChunkThenRest_emitsToken()
{
    ai::SsePartialParser parser;
    QStringList tokens;
    parser.feed("event: content_block_delta\ndata: {\"type\":\"content_block_delta\","
                "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hel",
                [&](const ai::SsePartialParser::Event &ev) {
        if (ev.kind == ai::SsePartialParser::EventKind::Token)
            tokens.append(ev.text);
    });
    QCOMPARE(tokens.size(), 0);
    parser.feed("lo\"}}\n\n", [&](const ai::SsePartialParser::Event &ev) {
        if (ev.kind == ai::SsePartialParser::EventKind::Token)
            tokens.append(ev.text);
    });
    QCOMPARE(tokens, QStringList{QStringLiteral("Hello")});
}

void TestSsePartialParser::anthropicToolUse_startThenInputJsonDeltas()
{
    const QList<Ev> evs = feedAll(
        "data: {\"type\":\"content_block_start\",\"index\":1,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"submit_commit_message\",\"input\":{}}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"type\\\":\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"fix\\\"}\"}}\n\n");
    QCOMPARE(evs.size(), 3);
    QCOMPARE(evs.at(0).kind, Kind::ToolDelta);
    QCOMPARE(evs.at(0).toolIndex, 1);
    QCOMPARE(evs.at(0).toolName, QStringLiteral("submit_commit_message"));
    QVERIFY(evs.at(0).text.isEmpty());
    QCOMPARE(evs.at(1).kind, Kind::ToolDelta);
    QCOMPARE(evs.at(1).text, QStringLiteral("{\"type\":"));
    QCOMPARE(evs.at(2).text, QStringLiteral("\"fix\"}"));
    // An input_json_delta must never be mistaken for text.
    for (const Ev &ev : evs)
        QVERIFY(ev.kind != Kind::Token);
}

void TestSsePartialParser::anthropicMessageDelta_emitsStopReason()
{
    const QList<Ev> evs = feedAll(
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n");
    QCOMPARE(evs.size(), 1);
    QCOMPARE(evs.at(0).kind, Kind::StopReason);
    QCOMPARE(evs.at(0).text, QStringLiteral("tool_use"));
}

void TestSsePartialParser::openaiToolCalls_nameThenArgumentFragments()
{
    const QList<Ev> evs = feedAll(
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"type\":\"function\","
        "\"function\":{\"name\":\"submit_commit_message\",\"arguments\":\"\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"{\\\"subject\\\":\"}}]}}]}\n\n");
    QCOMPARE(evs.size(), 2);
    QCOMPARE(evs.at(0).kind, Kind::ToolDelta);
    QCOMPARE(evs.at(0).toolName, QStringLiteral("submit_commit_message"));
    QCOMPARE(evs.at(1).kind, Kind::ToolDelta);
    QVERIFY(evs.at(1).toolName.isEmpty());
    QCOMPARE(evs.at(1).text, QStringLiteral("{\"subject\":"));
}

void TestSsePartialParser::openaiFinishReason_emitsStopReasonBeforeDone()
{
    const QList<Ev> evs = feedAll(
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n");
    QCOMPARE(evs.size(), 2);
    QCOMPARE(evs.at(0).kind, Kind::StopReason);
    QCOMPARE(evs.at(0).text, QStringLiteral("length"));
    QCOMPARE(evs.at(1).kind, Kind::Done);
}

void TestSsePartialParser::openaiToolCallWithoutIndex_usesArrayPosition()
{
    const QList<Ev> evs = feedAll(
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"a\",\"arguments\":\"{}\"}},"
        "{\"function\":{\"name\":\"b\",\"arguments\":\"{}\"}}]}}]}\n\n");
    QCOMPARE(evs.size(), 2);
    QCOMPARE(evs.at(0).toolIndex, 0);
    QCOMPARE(evs.at(1).toolIndex, 1);
}

QTEST_APPLESS_MAIN(TestSsePartialParser)

#include "test_sse_partial_parser.moc"
