/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * SPDX short: GPL-3.0-or-later
 */

#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QTextBrowser>
#include <QTextFrame>
#include <QToolButton>
#include <QStringList>


#include "AcpToolCallCard.h"


class TestAcpToolCallCard : public QObject
{
    Q_OBJECT

private slots:
    void grill_me_card_title_names_the_questions();
    void collapsed_card_defers_body_render_until_expanded();
    void expanded_card_coalesces_running_updates();
    void diff_card_auto_expands_at_terminal_status();
    void running_diff_card_stays_collapsed();
    void collapsed_multiline_title_autosizes_to_two_lines();
    void chatFont_change_refits_collapsed_header();
    void finishing_via_apply_matches_a_finished_card();
    void expanded_diff_taller_than_collapsed_while_hidden();
    void omp_pending_status_uses_running_glyph();
    void omp_in_progress_diff_stays_collapsed();
    void omp_write_title_from_kind_edit_content();
    void omp_edit_title_from_prefixed_path();
    void compact_numbered_diff_renders_without_old_new();
    void compact_unified_diff_renders_hunk();
    void edit_full_file_old_new_shows_only_hunk();
    void numbered_full_file_dump_collapses_to_hunk();
    void omp_hashline_text_not_shown_when_diff_present();
    void goalJudge_setsGoalAgentPropertyAndBadge();
    void subagent_instructions_sit_above_nested_calls();
    void nested_calls_are_compact_rows();
    void long_subagent_instructions_clamp_until_show_more();
    void subagent_cards_are_marked_apart_from_tools();
    void omp_task_card_names_its_agents_and_instructions();
    void omp_task_input_survives_history_slimming();
    void collapsed_subagent_card_shows_its_last_call();
    void subagent_rows_share_the_title_column();


};

namespace {

QTextBrowser *bodyFor(AcpToolCallCard &card)
{
    return card.findChild<QTextBrowser *>();
}

AcpProtocol::AcpToolCall baseCall()
{
    AcpProtocol::AcpToolCall tc;
    tc.id = QStringLiteral("tool-1");
    tc.title = QStringLiteral("Command");
    tc.status = QStringLiteral("running");
    return tc;
}

QJsonObject rawStdout(const QString &text)
{
    QJsonObject raw;
    raw.insert(QStringLiteral("stdout"), text);
    return raw;
}

QJsonObject diffBlock()
{
    QJsonObject diff;
    diff.insert(QStringLiteral("type"), QStringLiteral("diff"));
    diff.insert(QStringLiteral("path"), QStringLiteral("a.cpp"));
    diff.insert(QStringLiteral("oldText"), QStringLiteral("old\n"));
    diff.insert(QStringLiteral("newText"), QStringLiteral("new\n"));
    return diff;
}

QString glyphOf(AcpToolCallCard &card)
{
    const auto labels = card.findChildren<QLabel *>();
    for (QLabel *label : labels) {
        if (label->text().size() == 1) {
            return label->text();
        }
    }
    return {};
}

QString titleOf(AcpToolCallCard &card)
{
    const auto labels = card.findChildren<QLabel *>();
    for (QLabel *label : labels) {
        if (label->text().size() > 1) {
            return label->text();
        }
    }
    return {};
}


} // namespace

void TestAcpToolCallCard::grill_me_card_title_names_the_questions()
{
    auto titleOf = [](const AcpProtocol::AcpToolCall &tc) {
        AcpToolCallCard card(tc);
        card.resize(800, 120); // the title elides to the label width
        card.show();
        QCoreApplication::processEvents();
        QStringList texts;
        for (const QLabel *label : card.findChildren<QLabel *>())
            texts.append(label->text());
        return texts.join(QLatin1Char('|'));
    };
    AcpProtocol::AcpToolCall tc;
    tc.id = QStringLiteral("ask1");
    tc.title = QStringLiteral("mcp__grill_me__ask");
    tc.status = QStringLiteral("completed");
    QJsonArray questions;
    for (int i = 0; i < 3; ++i)
        questions.append(QJsonObject{{QStringLiteral("title"), QStringLiteral("q")}});
    tc.rawInput = QJsonObject{{QStringLiteral("questions"), questions}};
    QVERIFY2(titleOf(tc).contains(QLatin1String("Asked you 3 questions")), qPrintable(titleOf(tc)));
    QVERIFY(!titleOf(tc).contains(QLatin1String("mcp__")));

    tc.rawInput.insert(QStringLiteral("title"), QStringLiteral("Round 2"));
    QVERIFY(titleOf(tc).contains(QLatin1String("Asked you: Round 2")));
}

void TestAcpToolCallCard::collapsed_card_defers_body_render_until_expanded()
{
    AcpProtocol::AcpToolCall tc = baseCall();
    tc.rawOutput = rawStdout(QStringLiteral("initial output"));

    AcpToolCallCard card(tc);
    card.resize(480, 120);
    // A collapsed card does not even build its body widget.
    QVERIFY(card.isCollapsed());
    QVERIFY(!bodyFor(card));

    AcpProtocol::AcpToolCallUpdate update;
    update.id = tc.id;
    update.status = QStringLiteral("completed");
    update.rawOutput = rawStdout(QStringLiteral("final output"));
    card.apply(update);

    QTest::qWait(120);
    QVERIFY(!bodyFor(card));

    card.setCollapsed(false);
    QTextBrowser *body = bodyFor(card);
    QVERIFY(body);
    QVERIFY(body->toPlainText().contains(QStringLiteral("final output")));

    // Collapse keeps the built body, hidden.
    card.setCollapsed(true);
    QCOMPARE(bodyFor(card), body);
    QVERIFY(body->isHidden());
}

void TestAcpToolCallCard::expanded_card_coalesces_running_updates()
{
    AcpProtocol::AcpToolCall tc = baseCall();
    AcpToolCallCard card(tc);
    card.resize(480, 120);

    card.setCollapsed(false);
    QTextBrowser *body = bodyFor(card);
    QVERIFY(body);
    QVERIFY(body->toPlainText().isEmpty());

    AcpProtocol::AcpToolCallUpdate first;
    first.id = tc.id;
    first.status = QStringLiteral("running");
    first.rawOutput = rawStdout(QStringLiteral("first output"));
    card.apply(first);
    QVERIFY(!body->toPlainText().contains(QStringLiteral("first output")));

    AcpProtocol::AcpToolCallUpdate second;
    second.id = tc.id;
    second.status = QStringLiteral("running");
    second.rawOutput = rawStdout(QStringLiteral("second output"));
    card.apply(second);
    QVERIFY(!body->toPlainText().contains(QStringLiteral("second output")));

    QTRY_VERIFY(body->toPlainText().contains(QStringLiteral("second output")));
    QVERIFY(!body->toPlainText().contains(QStringLiteral("first output")));
}

void TestAcpToolCallCard::diff_card_auto_expands_at_terminal_status()
{
    // A diff card arriving already-completed must auto-expand so the user sees
    // the change without clicking. The render still rides the debounce path.
    AcpProtocol::AcpToolCall tc = baseCall();
    tc.status = QStringLiteral("completed");
    tc.content.append(diffBlock());

    AcpToolCallCard card(tc);
    card.resize(480, 120);
    QTextBrowser *body = bodyFor(card);
    QVERIFY(body);

    QVERIFY(!card.isCollapsed());
    QVERIFY(card.shouldPreserveExpanded());  // survives onTurnEnded() re-collapse
    QTRY_VERIFY(body->toPlainText().contains(QStringLiteral("a.cpp")));
}

void TestAcpToolCallCard::running_diff_card_stays_collapsed()
{
    // Diff content present but tool still running: must NOT expand or render —
    // expanding mid-stream drags the diff render into the update hot path.
    AcpProtocol::AcpToolCall tc = baseCall();
    tc.status = QStringLiteral("running");
    tc.content.append(diffBlock());

    AcpToolCallCard card(tc);
    card.resize(480, 120);

    QVERIFY(card.isCollapsed());
    QTest::qWait(120);
    QVERIFY(!bodyFor(card));

    // Once it finishes, it auto-expands and renders.
    AcpProtocol::AcpToolCallUpdate update;
    update.id = tc.id;
    update.status = QStringLiteral("completed");
    card.apply(update);

    QVERIFY(!card.isCollapsed());
    QTextBrowser *body = bodyFor(card);
    QVERIFY(body);
    QTRY_VERIFY(body->toPlainText().contains(QStringLiteral("a.cpp")));
}

// A bigger chat font must grow a collapsed one-line card at once (no resize
// follows a font change), not keep the header row's old cached height.
void TestAcpToolCallCard::chatFont_change_refits_collapsed_header()
{
    AcpProtocol::AcpToolCall call = baseCall();
    call.title = QStringLiteral("python3 script.py");
    AcpToolCallCard card(call);
    card.resize(480, 40);
    QVERIFY(card.isCollapsed());
    const int before = card.height();

    QFont big = card.font();
    big.setPixelSize(48);
    card.setChatFont(big);
    QVERIFY2(card.height() > before,
             qPrintable(QStringLiteral("%1 -> %2").arg(before).arg(card.height())));
}

// The final status swaps the glyph and shows the duration: a collapsed card
// that finishes through apply() must end as tall as one built finished.
void TestAcpToolCallCard::finishing_via_apply_matches_a_finished_card()
{
    AcpProtocol::AcpToolCall running = baseCall();
    running.status = QStringLiteral("pending");
    AcpToolCallCard live(running);
    live.resize(480, 40);
    QVERIFY(live.isCollapsed());

    AcpProtocol::AcpToolCallUpdate done;
    done.id = running.id;
    done.status = QStringLiteral("completed");
    done.durationMs = 1234;
    live.apply(done);

    AcpProtocol::AcpToolCall finished = baseCall();
    finished.status = QStringLiteral("completed");
    finished.durationMs = 1234;
    AcpToolCallCard built(finished);
    built.resize(480, 40);
    QCOMPARE(live.height(), built.height());
}

void TestAcpToolCallCard::collapsed_multiline_title_autosizes_to_two_lines()
{
    // A one-line command keeps the original collapsed height. Two (or more)
    // lines must grow the header so the second line is not clipped — including
    // on first paint, not only after expand→collapse.
    AcpProtocol::AcpToolCall oneLine = baseCall();
    oneLine.title = QStringLiteral("python3 script.py");
    AcpToolCallCard one(oneLine);
    one.resize(480, 40);
    QVERIFY(one.isCollapsed());
    const int h1 = one.height();

    AcpProtocol::AcpToolCall twoLine = baseCall();
    twoLine.title = QStringLiteral("python3 -c \"\nimport hashlib");
    AcpToolCallCard two(twoLine);
    two.resize(480, 40);
    QVERIFY(two.isCollapsed());
    const int h2 = two.height();
    QVERIFY(h2 > h1);

    AcpProtocol::AcpToolCall threeLine = baseCall();
    threeLine.title = QStringLiteral("python3 -c \"\nimport hashlib\nprint(1)");
    AcpToolCallCard three(threeLine);
    three.resize(480, 40);
    QCOMPARE(three.height(), h2);

    // Title arriving via apply() must grow a card that started as one line.
    AcpProtocol::AcpToolCallUpdate update;
    update.id = oneLine.id;
    update.title = twoLine.title;
    one.apply(update);
    QVERIFY(one.height() > h1);

    // Expand→collapse must not change the two-line height (the previously-
    // working path).
    const int beforeToggle = two.height();
    two.setCollapsed(false);
    two.setCollapsed(true);
    QCOMPARE(two.height(), beforeToggle);
}

void TestAcpToolCallCard::expanded_diff_taller_than_collapsed_while_hidden()
{
    // Inactive session tabs live on a hidden QStackedWidget page. An
    // auto-expanded diff must still include the body in its pinned height
    // even though isVisible() is false — otherwise switching back clips
    // the diff to the header.
    AcpProtocol::AcpToolCall tc = baseCall();
    tc.status = QStringLiteral("completed");
    tc.content.append(diffBlock());

    AcpToolCallCard expanded(tc);
    expanded.resize(480, 120);
    QVERIFY(!expanded.isVisible());
    QVERIFY(!expanded.isCollapsed());
    QTRY_VERIFY(bodyFor(expanded)->toPlainText().contains(QStringLiteral("a.cpp")));
    const int expandedH = expanded.height();

    AcpProtocol::AcpToolCall running = baseCall();
    AcpToolCallCard collapsed(running);
    collapsed.resize(480, 120);
    QVERIFY(collapsed.isCollapsed());
    const int collapsedH = collapsed.height();

    QVERIFY2(expandedH > collapsedH,
             qPrintable(QStringLiteral("expanded=%1 collapsed=%2")
                            .arg(expandedH)
                            .arg(collapsedH)));
}

void TestAcpToolCallCard::omp_pending_status_uses_running_glyph()
{
    AcpProtocol::AcpToolCall tc = baseCall();
    tc.status = QStringLiteral("pending");
    AcpToolCallCard pending(tc);
    pending.resize(480, 120);
    QCOMPARE(glyphOf(pending), QStringLiteral("⚙"));

    AcpProtocol::AcpToolCallUpdate up;
    up.id = tc.id;
    up.status = QStringLiteral("in_progress");
    pending.apply(up);
    QCOMPARE(glyphOf(pending), QStringLiteral("⚙"));
}

void TestAcpToolCallCard::omp_in_progress_diff_stays_collapsed()
{
    AcpProtocol::AcpToolCall tc = baseCall();
    tc.status = QStringLiteral("in_progress");
    tc.content.append(diffBlock());
    AcpToolCallCard card(tc);
    card.resize(480, 120);
    QVERIFY(card.isCollapsed());
    QTest::qWait(120);
    QVERIFY(!bodyFor(card));
}

void TestAcpToolCallCard::omp_write_title_from_kind_edit_content()
{
    AcpProtocol::AcpToolCall tc = baseCall();
    tc.title = QStringLiteral("edit: D:/tmp/test-git/probe.txt");
    tc.kind = QStringLiteral("edit");
    tc.rawInput.insert(QStringLiteral("path"), QStringLiteral("D:/tmp/test-git/probe.txt"));
    tc.rawInput.insert(QStringLiteral("content"), QStringLiteral("hello\n"));
    AcpToolCallCard card(tc);
    card.resize(800, 80);
    const QString title = titleOf(card);
    QVERIFY2(title.contains(QLatin1String("Write:")), qPrintable(title));
    QVERIFY2(!title.contains(QLatin1String("Edit:")), qPrintable(title));

}

void TestAcpToolCallCard::omp_edit_title_from_prefixed_path()
{
    AcpProtocol::AcpToolCall tc = baseCall();
    tc.title = QStringLiteral("edit: conflict.txt");
    tc.kind = QStringLiteral("edit");
    tc.rawInput.insert(QStringLiteral("path"), QStringLiteral("conflict.txt"));
    tc.rawInput.insert(QStringLiteral("oldString"), QStringLiteral("a"));
    tc.rawInput.insert(QStringLiteral("newString"), QStringLiteral("b"));
    AcpToolCallCard card(tc);
    card.resize(800, 80);
    QVERIFY(titleOf(card).contains(QStringLiteral("Edit: conflict.txt")));
}

void TestAcpToolCallCard::compact_numbered_diff_renders_without_old_new()
{
    AcpProtocol::AcpToolCall tc = baseCall();
    tc.status = QStringLiteral("completed");
    QJsonObject block;
    block.insert(QStringLiteral("type"), QStringLiteral("diff"));
    block.insert(QStringLiteral("path"), QStringLiteral("numbered.cpp"));
    block.insert(QStringLiteral("diff"),
                 QStringLiteral("  10|keep\n-11|old line\n+11|new line\n"));
    tc.content.append(block);
    AcpToolCallCard card(tc);
    card.resize(480, 200);
    QVERIFY(!card.isCollapsed());
    QTRY_VERIFY(bodyFor(card)->toPlainText().contains(QStringLiteral("numbered.cpp")));
    QVERIFY(bodyFor(card)->toPlainText().contains(QStringLiteral("keep")));
    QVERIFY(bodyFor(card)->toPlainText().contains(QStringLiteral("old line")));
    QVERIFY(bodyFor(card)->toPlainText().contains(QStringLiteral("new line")));
}

void TestAcpToolCallCard::compact_unified_diff_renders_hunk()
{
    AcpProtocol::AcpToolCall tc = baseCall();
    tc.status = QStringLiteral("completed");
    QJsonObject block;
    block.insert(QStringLiteral("type"), QStringLiteral("diff"));
    block.insert(QStringLiteral("path"), QStringLiteral("unified.cpp"));
    block.insert(QStringLiteral("diff"),
                 QStringLiteral("@@ -1,2 +1,2 @@\n keep\n-old\n+new\n"));
    tc.content.append(block);
    AcpToolCallCard card(tc);
    card.resize(480, 200);
    QVERIFY(!card.isCollapsed());
    QTRY_VERIFY(bodyFor(card)->toPlainText().contains(QStringLiteral("unified.cpp")));
    QVERIFY(bodyFor(card)->toPlainText().contains(QStringLiteral("keep")));
    QVERIFY(bodyFor(card)->toPlainText().contains(QStringLiteral("old")));
    QVERIFY(bodyFor(card)->toPlainText().contains(QStringLiteral("new")));
}

void TestAcpToolCallCard::edit_full_file_old_new_shows_only_hunk()
{
    QStringList oldLines;
    QStringList newLines;
    oldLines.reserve(40);
    newLines.reserve(40);
    for (int i = 1; i <= 40; ++i) {
        const QString line = QStringLiteral("line-%1").arg(i, 2, 10, QLatin1Char('0'));
        oldLines.append(line);
        newLines.append(line);
    }
    newLines[19] = QStringLiteral("changed-20");

    AcpProtocol::AcpToolCall tc = baseCall();
    tc.status = QStringLiteral("completed");
    QJsonObject block;
    block.insert(QStringLiteral("type"), QStringLiteral("diff"));
    block.insert(QStringLiteral("path"), QStringLiteral("big.cpp"));
    block.insert(QStringLiteral("oldText"), oldLines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    block.insert(QStringLiteral("newText"), newLines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    tc.content.append(block);
    AcpToolCallCard card(tc);
    card.resize(480, 400);
    QVERIFY(!card.isCollapsed());
    QTRY_VERIFY(bodyFor(card)->toPlainText().contains(QStringLiteral("changed-20")));
    const QString body = bodyFor(card)->toPlainText();
    QVERIFY(body.contains(QStringLiteral("line-20")));
    QVERIFY(body.contains(QStringLiteral("...")));
    QVERIFY(!body.contains(QStringLiteral("line-05")));
    QVERIFY(!body.contains(QStringLiteral("line-35")));
}

void TestAcpToolCallCard::numbered_full_file_dump_collapses_to_hunk()
{
    QString dump;
    dump.reserve(40 * 24);
    for (int i = 1; i <= 40; ++i) {
        if (i == 20) {
            dump += QStringLiteral("-20|line-20\n+20|changed-20\n");
        } else {
            dump += QStringLiteral("  %1|line-%2\n")
                        .arg(i, 2, 10, QLatin1Char('0'))
                        .arg(i, 2, 10, QLatin1Char('0'));
        }
    }

    AcpProtocol::AcpToolCall tc = baseCall();
    tc.status = QStringLiteral("completed");
    QJsonObject block;
    block.insert(QStringLiteral("type"), QStringLiteral("diff"));
    block.insert(QStringLiteral("path"), QStringLiteral("numbered.cpp"));
    block.insert(QStringLiteral("diff"), dump);
    tc.content.append(block);
    AcpToolCallCard card(tc);
    card.resize(480, 400);
    QVERIFY(!card.isCollapsed());
    QTRY_VERIFY(bodyFor(card)->toPlainText().contains(QStringLiteral("changed-20")));
    const QString body = bodyFor(card)->toPlainText();
    QVERIFY(body.contains(QStringLiteral("...")));
    QVERIFY(!body.contains(QStringLiteral("line-05")));
    QVERIFY(!body.contains(QStringLiteral("line-35")));
}

void TestAcpToolCallCard::omp_hashline_text_not_shown_when_diff_present()
{
    AcpProtocol::AcpToolCall tc = baseCall();
    tc.status = QStringLiteral("completed");
    QJsonObject dump;
    dump.insert(QStringLiteral("type"), QStringLiteral("text"));
    dump.insert(QStringLiteral("text"),
                QStringLiteral("[test.txt#86C8]\n1:test\n2:# edited\n3:2\n4:3\n"));
    QJsonObject diff;
    diff.insert(QStringLiteral("type"), QStringLiteral("diff"));
    diff.insert(QStringLiteral("path"), QStringLiteral("test.txt"));
    diff.insert(QStringLiteral("oldText"), QStringLiteral("test\n2\n3\n"));
    diff.insert(QStringLiteral("newText"), QStringLiteral("test\n# edited\n2\n3\n"));
    tc.content.append(dump);
    tc.content.append(diff);
    AcpToolCallCard card(tc);
    card.resize(480, 400);
    QVERIFY(!card.isCollapsed());
    QTRY_VERIFY(bodyFor(card)->toPlainText().contains(QStringLiteral("# edited")));
    const QString body = bodyFor(card)->toPlainText();
    QVERIFY(!body.contains(QStringLiteral("[test.txt#")));
    QVERIFY(!body.contains(QStringLiteral("86C8")));
}

void TestAcpToolCallCard::goalJudge_setsGoalAgentPropertyAndBadge()
{
    AcpToolCallCard card(baseCall());
    QVERIFY(!card.property("goalAgent").toBool());
    QVERIFY(card.isCollapsed());
    card.setFromGoalAgent(true);
    QCOMPARE(card.property("goalAgent").toBool(), true);
    QVERIFY(card.isFromGoalAgent());
    QVERIFY(card.isCollapsed());
    bool found = false;
    for (QLabel *label : card.findChildren<QLabel *>()) {
        if (label->text() == QStringLiteral("Goal")) {
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

namespace {

AcpProtocol::AcpToolCall agentCall(const QString &prompt)
{
    AcpProtocol::AcpToolCall tc;
    tc.id = QStringLiteral("agent-1");
    tc.title = QStringLiteral("Task");
    tc.status = QStringLiteral("completed");
    tc.rawInput = QJsonObject{{QStringLiteral("subagent_type"), QStringLiteral("Explore")},
                              {QStringLiteral("description"), QStringLiteral("Spec review")},
                              {QStringLiteral("prompt"), prompt}};
    tc.content = QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                        {QStringLiteral("text"), QStringLiteral("**Done.**")}}};
    return tc;
}

AcpProtocol::AcpToolCall childCall(int i)
{
    AcpProtocol::AcpToolCall tc = baseCall();
    tc.id = QStringLiteral("child-%1").arg(i);
    tc.status = QStringLiteral("completed");
    tc.rawInput = QJsonObject{{QStringLiteral("command"), QStringLiteral("ls")}};
    return tc;
}

} // namespace

void TestAcpToolCallCard::subagent_instructions_sit_above_nested_calls()
{
    AcpToolCallCard card(agentCall(QStringLiteral("Review the **diff**.")));
    card.setSubagentActivity(2, QString());
    card.resize(600, 40);
    card.show();
    card.setCollapsed(false);
    card.addChildCard(new AcpToolCallCard(childCall(1)));
    card.addChildCard(new AcpToolCallCard(childCall(2)));
    QCoreApplication::processEvents();

    auto *prompt = card.findChild<QWidget *>(QStringLiteral("acpSubagentPrompt"));
    auto *children = card.findChild<QFrame *>(QStringLiteral("acpSubagentChildren"));
    QVERIFY(prompt && children);
    QVERIFY(!prompt->isHidden());
    auto *promptText = prompt->findChild<QTextBrowser *>();
    QVERIFY(promptText);
    // Rendered as markdown: no literal asterisks.
    QCOMPARE(promptText->toPlainText().trimmed(), QStringLiteral("Review the diff."));

    // The result: the card's own body, below the nested calls.
    QTextBrowser *result = nullptr;
    for (QTextBrowser *b : card.findChildren<QTextBrowser *>(Qt::FindDirectChildrenOnly))
        result = b;
    QVERIFY(result);
    QCOMPARE(result->toPlainText().trimmed(), QStringLiteral("Done."));

    QLayout *outer = card.layout();
    QVERIFY(outer->indexOf(prompt) < outer->indexOf(children));
    QVERIFY(outer->indexOf(children) < outer->indexOf(result));
    // The card is tall enough for every row it shows.
    QVERIFY(card.height() >= prompt->height() + children->sizeHint().height() + result->height());

    card.setCollapsed(true);
    QVERIFY(prompt->isHidden());
    QVERIFY(children->isHidden());
}

void TestAcpToolCallCard::nested_calls_are_compact_rows()
{
    AcpToolCallCard standalone(childCall(0));
    standalone.resize(600, 40);
    standalone.show();
    QCoreApplication::processEvents();

    AcpToolCallCard card(agentCall(QString()));
    card.setSubagentActivity(1, QString());
    card.resize(600, 40);
    card.show();
    card.setCollapsed(false);
    auto *child = new AcpToolCallCard(childCall(1));
    card.addChildCard(child);
    QCoreApplication::processEvents();

    QVERIFY2(child->height() < standalone.height(),
             qPrintable(QStringLiteral("%1 vs %2").arg(child->height()).arg(standalone.height())));
    // No instructions: no empty section.
    QVERIFY(!card.findChild<QWidget *>(QStringLiteral("acpSubagentPrompt")));
}

void TestAcpToolCallCard::long_subagent_instructions_clamp_until_show_more()
{
    QStringList lines;
    for (int i = 0; i < 40; ++i)
        lines.append(QStringLiteral("Step %1.").arg(i));
    AcpToolCallCard card(agentCall(lines.join(QStringLiteral("\n\n"))));
    card.resize(600, 40);
    card.show();
    card.setCollapsed(false);
    QCoreApplication::processEvents();

    auto *prompt = card.findChild<QWidget *>(QStringLiteral("acpSubagentPrompt"));
    auto *more = card.findChild<QToolButton *>(QStringLiteral("acpSubagentPromptMore"));
    QVERIFY(prompt && more);
    QVERIFY(!more->isHidden());
    auto *promptText = prompt->findChild<QTextBrowser *>();
    const int clamped = promptText->height();
    const int collapsedCard = card.height();

    more->click();
    QCoreApplication::processEvents();
    QVERIFY(promptText->height() > clamped * 3);
    QVERIFY(card.height() > collapsedCard);

    more->click();
    QCoreApplication::processEvents();
    QCOMPARE(promptText->height(), clamped);
}

namespace {

// omp's task tool, as omp sends its arguments in rawInput.
AcpProtocol::AcpToolCall ompTaskCall()
{
    QJsonArray tasks;
    for (int i = 1; i <= 3; ++i) {
        tasks.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("EchoAgent%1").arg(i)},
                                 {QStringLiteral("task"), QStringLiteral("Run echo %1.").arg(i)},
                                 {QStringLiteral("solutionSpace"), QStringLiteral("one fix")}});
    }
    AcpProtocol::AcpToolCall tc;
    tc.id = QStringLiteral("call_1|fc_1");
    tc.title = QStringLiteral("Spawning 3 echo subagents");
    tc.kind = QStringLiteral("other");
    tc.status = QStringLiteral("completed");
    tc.rawInput = QJsonObject{{QStringLiteral("context"), QStringLiteral("Shared goal.")},
                              {QStringLiteral("i"), QStringLiteral("Spawning 3 echo subagents")},
                              {QStringLiteral("tasks"), tasks}};
    return tc;
}

} // namespace

void TestAcpToolCallCard::subagent_cards_are_marked_apart_from_tools()
{
    // The bot icon, not a tint, tells a subagent card from a tool.
    auto marked = [](AcpToolCallCard &card) {
        auto *icon = card.findChild<QLabel *>(QStringLiteral("acpSubagentIcon"),
                                              Qt::FindDirectChildrenOnly);
        return icon && !icon->isHidden() && card.property("subagent").toBool();
    };
    AcpToolCallCard tool(childCall(0));
    QVERIFY(!marked(tool));

    AcpToolCallCard agent(agentCall(QStringLiteral("Look around.")));
    QVERIFY(marked(agent));
    AcpToolCallCard omp(ompTaskCall());
    QVERIFY(marked(omp));

    // Recognised late: the rawInput arrives with an update.
    AcpProtocol::AcpToolCall bare = agentCall(QString());
    bare.rawInput = {};
    AcpToolCallCard late(bare);
    QVERIFY(!marked(late));
    AcpProtocol::AcpToolCallUpdate update;
    update.rawInput = agentCall(QStringLiteral("Look around.")).rawInput;
    late.apply(update);
    QVERIFY(marked(late));

    // A nested call inside an Agent card is a plain row, not a subagent.
    agent.setCollapsed(false);
    auto *child = new AcpToolCallCard(childCall(1));
    agent.addChildCard(child);
    QVERIFY(!marked(*child));
}

// Collapsed, the card names the subagent's running call, and its last call
// once it is done.
void TestAcpToolCallCard::collapsed_subagent_card_shows_its_last_call()
{
    AcpProtocol::AcpToolCall tc = agentCall(QStringLiteral("Look around."));
    tc.status = QStringLiteral("in_progress");
    AcpToolCallCard card(tc);
    card.resize(600, 40);
    card.show();
    card.setSubagentActivity(1, QStringLiteral("Command: echo 1"));
    QCoreApplication::processEvents();
    auto lastCallLine = [&card]() -> QLabel * {
        for (QLabel *label : card.findChildren<QLabel *>(Qt::FindDirectChildrenOnly)) {
            if (label->text().startsWith(QStringLiteral("↳")))
                return label;
        }
        return nullptr;
    };
    QVERIFY(lastCallLine() && !lastCallLine()->isHidden());
    const int runningH = card.height();

    AcpProtocol::AcpToolCallUpdate done;
    done.status = QStringLiteral("completed");
    card.apply(done);
    card.setSubagentActivity(1, QStringLiteral("Command: echo 1"));
    QCoreApplication::processEvents();
    QVERIFY(lastCallLine() && !lastCallLine()->isHidden());
    QVERIFY(lastCallLine()->text().contains(QStringLiteral("echo 1")));
    QCOMPARE(card.height(), runningH);

    card.setCollapsed(false); // the nested rows take over
    QVERIFY(lastCallLine()->isHidden());
}

void TestAcpToolCallCard::omp_task_card_names_its_agents_and_instructions()
{
    const AcpProtocol::AcpToolCall tc = ompTaskCall();
    QCOMPARE(AcpToolCallCard::enrichedTitle(tc),
             QStringLiteral("Agent ×3: Spawning 3 echo subagents"));

    AcpProtocol::AcpToolCall one = tc;
    QJsonObject input = one.rawInput;
    input.insert(QStringLiteral("tasks"), QJsonArray{tc.rawInput.value(QStringLiteral("tasks")).toArray().first()});
    input.insert(QStringLiteral("agent"), QStringLiteral("scout"));
    one.rawInput = input;
    one.title.clear();
    QCOMPARE(AcpToolCallCard::enrichedTitle(one),
             QStringLiteral("Agent scout: Spawning 3 echo subagents"));

    AcpToolCallCard card(tc);
    card.resize(600, 40);
    card.show();
    card.setCollapsed(false);
    QCoreApplication::processEvents();
    auto *prompt = card.findChild<QWidget *>(QStringLiteral("acpSubagentPrompt"));
    QVERIFY(prompt && !prompt->isHidden());
    const QString text = prompt->findChild<QTextBrowser *>()->toPlainText();
    QVERIFY2(text.startsWith(QLatin1String("Shared goal.")), qPrintable(text));
    QVERIFY(text.contains(QLatin1String("EchoAgent2")));
    QVERIFY(text.contains(QLatin1String("Run echo 3.")));
    QVERIFY(!text.contains(QLatin1String("**")));
}

void TestAcpToolCallCard::omp_task_input_survives_history_slimming()
{
    const QJsonObject raw = ompTaskCall().rawInput;
    const QJsonObject kept = AcpProtocol::subagentInputForHistory(raw);
    QVERIFY(AcpProtocol::isSubagentToolInput(kept));
    QCOMPARE(AcpProtocol::subagentInstructions(kept), AcpProtocol::subagentInstructions(raw));
    QVERIFY(!kept.value(QStringLiteral("tasks")).toArray().first().toObject()
                 .contains(QStringLiteral("solutionSpace")));

    // WebFetch: a prompt, but no subagent.
    QVERIFY(!AcpProtocol::isSubagentToolInput(QJsonObject{
        {QStringLiteral("url"), QStringLiteral("https://example.com")},
        {QStringLiteral("prompt"), QStringLiteral("summarize")}}));
    // Bash: a description, but no prompt.
    QVERIFY(!AcpProtocol::isSubagentToolInput(QJsonObject{
        {QStringLiteral("command"), QStringLiteral("ls")},
        {QStringLiteral("description"), QStringLiteral("list")}}));
}

// Reported: the instructions, the nested calls and the result each started at
// a different x. Everything below the header starts where the title does.
void TestAcpToolCallCard::subagent_rows_share_the_title_column()
{
    AcpToolCallCard card(agentCall(QStringLiteral("Run echo.")));
    card.setSubagentActivity(1, QStringLiteral("Command: echo 1"));
    card.resize(600, 40);
    card.show();
    card.setCollapsed(false);
    auto *child = new AcpToolCallCard(childCall(1));
    card.addChildCard(child);
    QCoreApplication::processEvents();

    QLabel *title = nullptr;
    QLabel *resultCaption = nullptr;
    for (QLabel *label : card.findChildren<QLabel *>(Qt::FindDirectChildrenOnly)) {
        if (label->text().startsWith(QStringLiteral("Agent")))
            title = label;
        else if (label->text() == QStringLiteral("Result"))
            resultCaption = label;
    }
    QVERIFY(title && resultCaption);
    const int column = title->x();
    QVERIFY(column > 0);

    auto *prompt = card.findChild<QWidget *>(QStringLiteral("acpSubagentPrompt"));
    QTextBrowser *promptText = prompt->findChild<QTextBrowser *>();
    QCOMPARE(promptText->mapTo(&card, QPoint(0, 0)).x(), column);
    QCOMPARE(child->mapTo(&card, QPoint(0, 0)).x(), column);
    // The nested row's own glyph sits at its left edge: no inner margin.
    QCOMPARE(child->layout()->contentsMargins().left(), 0);
    QCOMPARE(resultCaption->x() + resultCaption->contentsMargins().left(), column);

    QTextBrowser *result = nullptr;
    for (QTextBrowser *b : card.findChildren<QTextBrowser *>(Qt::FindDirectChildrenOnly))
        result = b;
    QVERIFY(result);
    const qreal inset = result->document()->rootFrame()->frameFormat().leftMargin();
    QCOMPARE(result->x() + qRound(inset), column);

    // Collapsed: the last-call line starts there too.
    card.setCollapsed(true);
    QCoreApplication::processEvents();
    for (QLabel *label : card.findChildren<QLabel *>(Qt::FindDirectChildrenOnly)) {
        if (label->text().startsWith(QStringLiteral("↳")))
            QCOMPARE(label->x() + label->contentsMargins().left(), column);
    }
}

QTEST_MAIN(TestAcpToolCallCard)
#include "test_acp_tool_call_card.moc"
