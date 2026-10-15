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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "GrillMeForm.h"

using namespace GrillMeForm;

namespace {

QJsonObject obj(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

} // namespace

class TestGrillMeForm : public QObject
{
    Q_OBJECT

private slots:
    void parsesGrillRound();
    void rejectsInvalidInputWithReason();
    void acceptsLenientShapes();
    void acceptsOtherWaysToNameThePick();
    void formatsAnswersForTheAgent();
    void summaryLine();
    void reportsQuestionsToClarify();
    void changedAnswerShowsRecommendedLabels();
    void carriesAnswersOverById();
    void recognisesGrillMeToolCalls();
    void unwrapsNestedArguments();
    void recognisesOmpDeviceWrite();
    void infersRecommendedFromRecommendation();
    void overviewListsEachAnswer();
    void formNoteFollowsTheAnswers();
    void hintsQuestionRoundCommands();
};

void TestGrillMeForm::parsesGrillRound()
{
    const QJsonObject args = obj(R"({"questions":[
        {"id":"Q1","title":"Architecture","body":"Pick **one**.","type":"single",
         "options":[{"value":"a","label":"Elicitation","description":"standard"},
                    {"value":"b2","label":"No-op tool"}],
         "recommended":"b2","recommendation":"Works with all agents."},
        {"id":"Q2","title":"Controls","type":"multi",
         "options":[{"value":"radio"},{"value":"checkbox"},{"value":"textarea"}],
         "recommended":["radio","checkbox"]},
        {"id":"Q3","title":"Module layout","type":"proposal","body":"```\nsrc/core\n```"},
        {"id":"Q4","title":"Scan inside?","type":"confirm","recommended":false,
         "yesLabel":"Scan","noLabel":"Skip"},
        {"id":"Q5","title":"Describe it","type":"textarea","required":true},
        {"id":"Q6","title":"Days","type":"number","min":0,"max":365,"recommended":30}
    ]})");
    QString error;
    const auto spec = parseToolInput(args, &error);
    QVERIFY2(spec.has_value(), qPrintable(error));
    QCOMPARE(spec->questions.size(), 6);

    const Question &q1 = spec->questions.at(0);
    QCOMPARE(q1.id, QStringLiteral("Q1"));
    QCOMPARE(q1.type, QuestionType::Single);
    QCOMPARE(q1.options.size(), 2);
    QCOMPARE(q1.options.at(1).label, QStringLiteral("No-op tool"));
    QCOMPARE(q1.recommended.toString(), QStringLiteral("b2"));
    QCOMPARE(q1.recommendation, QStringLiteral("Works with all agents."));
    QVERIFY(q1.allowOther);   // default for choice questions
    QVERIFY(!q1.required);    // default

    const Question &q2 = spec->questions.at(1);
    QCOMPARE(q2.options.at(0).label, QStringLiteral("radio")); // label defaults to value
    QCOMPARE(q2.recommended.toArray().size(), 2);

    const Question &q3 = spec->questions.at(2);
    QCOMPARE(q3.type, QuestionType::Proposal);
    QCOMPARE(q3.recommended.toString(), QStringLiteral("accept")); // proposal default
    QVERIFY(!q3.allowOther);

    const Question &q4 = spec->questions.at(3);
    QCOMPARE(q4.recommended, QJsonValue(false));
    QCOMPARE(q4.yesLabel, QStringLiteral("Scan"));

    QVERIFY(spec->questions.at(4).required);
    QCOMPARE(spec->questions.at(5).min.value_or(-1), 0.0);
    QCOMPARE(spec->questions.at(5).recommended.toDouble(), 30.0);
}

void TestGrillMeForm::rejectsInvalidInputWithReason()
{
    QString error;
    QVERIFY(!parseToolInput(obj(R"({"questions":[{"id":"Q7","title":"Pick","type":"single"}]})"), &error));
    QVERIFY2(error.contains(QLatin1String("Q7")) && error.contains(QLatin1String("options")), qPrintable(error));

    QVERIFY(!parseToolInput(obj(R"({"questions":[]})"), &error));
    QVERIFY(!parseToolInput(obj(R"({"questions":[{"id":"A","title":"x","type":"slider"}]})"), &error));
    QVERIFY(error.contains(QLatin1String("slider")));
    QVERIFY(!parseToolInput(obj(R"({"questions":[{"id":"A","title":"x","type":"single",
        "options":["a","b"],"recommended":"c"}]})"), &error));
    QVERIFY(error.contains(QLatin1String("\"c\"")));
    QVERIFY(!parseToolInput(obj(R"({"questions":[{"id":"A","title":"x","type":"text"},
        {"id":"A","title":"y","type":"text"}]})"), &error));
    QVERIFY(error.contains(QLatin1String("duplicate")));

    QJsonArray many;
    for (int i = 0; i < kMaxQuestions + 1; ++i)
        many.append(QJsonObject{{QStringLiteral("title"), QStringLiteral("t")}, {QStringLiteral("type"), QStringLiteral("text")}});
    QVERIFY(!parseToolInput(QJsonObject{{QStringLiteral("questions"), many}}, &error));
}

void TestGrillMeForm::acceptsLenientShapes()
{
    // questions as a JSON string, no ids, options as bare strings, lone string
    // for multi, numeric string for number, "yes" for confirm.
    const QJsonObject args{{QStringLiteral("questions"), QStringLiteral(R"([
        {"title":"Pick","type":"multi","options":["x","y"],"recommended":"y"},
        {"title":"Count","type":"number","recommended":"12"},
        {"title":"Ok?","type":"confirm","recommended":"yes"}])")}};
    QString error;
    const auto spec = parseToolInput(args, &error);
    QVERIFY2(spec.has_value(), qPrintable(error));
    QCOMPARE(spec->questions.at(0).id, QStringLiteral("Q1"));
    QCOMPARE(spec->questions.at(2).id, QStringLiteral("Q3"));
    QCOMPARE(spec->questions.at(0).recommended.toArray(), QJsonArray{QStringLiteral("y")});
    QCOMPARE(spec->questions.at(1).recommended.toDouble(), 12.0);
    QCOMPARE(spec->questions.at(2).recommended, QJsonValue(true));
}

// Agents name the pick in other ways than `recommended` set to a value.
void TestGrillMeForm::acceptsOtherWaysToNameThePick()
{
    QString error;
    const auto spec = parseToolInput(obj(R"({"questions":[
        {"id":"Q1","title":"Alias","type":"single","options":["x","y"],"default":"y"},
        {"id":"Q2","title":"Flag","type":"single",
         "options":[{"value":"x"},{"value":"y","recommended":true}]},
        {"id":"Q3","title":"Flags","type":"multi",
         "options":[{"value":"x","recommended":true},{"value":"y"},{"value":"z","recommended":true}]},
        {"id":"Q4","title":"Label","type":"single",
         "options":[{"value":"a","label":"Keep it"},{"value":"b","label":"Drop it"}],"recommended":"drop it"},
        {"id":"Q5","title":"Case","type":"multi","options":["Radio","Checkbox"],"recommended":[" radio"]},
        {"id":"Q6","title":"Blank","type":"single","options":["x","y"],"recommended":"","suggested":"x"}
    ]})"), &error);
    QVERIFY2(spec.has_value(), qPrintable(error));
    QCOMPARE(spec->questions.at(0).recommended.toString(), QStringLiteral("y"));
    QCOMPARE(spec->questions.at(1).recommended.toString(), QStringLiteral("y"));
    QCOMPARE(spec->questions.at(2).recommended.toArray(), QJsonArray({QStringLiteral("x"), QStringLiteral("z")}));
    QCOMPARE(spec->questions.at(3).recommended.toString(), QStringLiteral("b"));
    QCOMPARE(spec->questions.at(4).recommended.toArray(), QJsonArray{QStringLiteral("Radio")});
    QCOMPARE(spec->questions.at(5).recommended.toString(), QStringLiteral("x"));

    // A value that names no option lists the ones that do.
    QVERIFY(!parseToolInput(obj(R"({"questions":[{"id":"A","title":"x","type":"single",
        "options":["a","b"],"recommended":"c"}]})"), &error));
    QVERIFY2(error.contains(QLatin1String("use one of: \"a\", \"b\"")), qPrintable(error));
}

namespace {

Spec grillSpec()
{
    const auto spec = parseToolInput(obj(R"({"title":"Round 2","questions":[
        {"id":"Q1","title":"Architecture","type":"single",
         "options":[{"value":"a","label":"Elicitation"},{"value":"b2","label":"No-op tool"}],
         "recommended":"b2"},
        {"id":"Q2","title":"Controls","type":"multi",
         "options":["radio","checkbox","textarea"],"recommended":["radio","checkbox"]},
        {"id":"Q3","title":"Module layout","type":"proposal"},
        {"id":"Q4","title":"Scan inside?","type":"confirm","recommended":false,
         "yesLabel":"Scan","noLabel":"Skip"},
        {"id":"Q5","title":"Describe it","type":"textarea"},
        {"id":"Q6","title":"Days","type":"number","recommended":30}
    ]})"));
    return *spec;
}

Answer answer(const char *id, const QJsonValue &value, const char *other = "", const char *note = "")
{
    return Answer{QString::fromUtf8(id), value, QString::fromUtf8(other), QString::fromUtf8(note)};
}

} // namespace

void TestGrillMeForm::formatsAnswersForTheAgent()
{
    const Spec spec = grillSpec();
    const QVector<Answer> answers{
        answer("Q1", QStringLiteral("b2")),
        answer("Q2", QJsonArray{QStringLiteral("radio"), QStringLiteral("textarea")}, "slider", "keep it small"),
        answer("Q3", QStringLiteral("change"), "move core to lib/"),
        answer("Q4", false),
        answer("Q5", QJsonValue()),
        answer("Q6", 45.0),
    };
    const QString expected = QStringLiteral(
        "[grill_me answers]\n"
        "Form: Round 2\n"
        "Q1 \"Architecture\": b2 (No-op tool) (recommended)\n"
        "Q2 \"Controls\": radio, textarea, other: \"slider\" — changed (recommended: radio, checkbox)\n"
        "  note: keep it small\n"
        "Q3 \"Module layout\": change requested: \"move core to lib/\" — changed (recommended: accept)\n"
        "Q4 \"Scan inside?\": no (Skip) (recommended)\n"
        "Q5 \"Describe it\": no answer\n"
        "Q6 \"Days\": 45 — changed (recommended: 30)\n");
    QCOMPARE(formatAnswers(spec, answers), expected);
}

void TestGrillMeForm::summaryLine()
{
    const Spec spec = grillSpec();
    QVector<Answer> answers = defaultAnswers(spec);
    QCOMPARE(answers.size(), 6);
    QCOMPARE(answers.at(0).value.toString(), QStringLiteral("b2"));
    QVERIFY(answers.at(4).value.isUndefined() || answers.at(4).value.isNull());
    // Q5 has no recommendation: 5 of 6 answered, none changed.
    QCOMPARE(answerSummary(spec, answers), QStringLiteral("Answered 5 of 6 questions · 0 changed"));

    answers[4].value = QStringLiteral("free text");
    answers[5].value = 7.0;
    const QString summary = answerSummary(spec, answers);
    QCOMPARE(summary, QStringLiteral("Answered 6 questions · 1 changed"));
    QCOMPARE(summaryFromAnswersText(formatAnswers(spec, answers)), summary);

    // Dismiss prompts share the collapsed bubble.
    QCOMPARE(summaryFromAnswersText(dismissPrompt(QString())), QStringLiteral("Dismissed the form"));
}

// A changed answer names the recommendation the way answers are named:
// choices and yes/no with their labels.
void TestGrillMeForm::changedAnswerShowsRecommendedLabels()
{
    const Spec spec = grillSpec();
    const QVector<Answer> answers{
        answer("Q1", QStringLiteral("a")),
        answer("Q2", QJsonArray{QStringLiteral("radio"), QStringLiteral("checkbox")}),
        answer("Q3", QStringLiteral("accept")),
        answer("Q4", true),
        answer("Q5", QJsonValue()),
        answer("Q6", 30.0),
    };
    const QString text = formatAnswers(spec, answers);
    QVERIFY2(text.contains(QStringLiteral("Q1 \"Architecture\": a (Elicitation) — changed (recommended: b2 (No-op tool))\n")),
             qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("Q4 \"Scan inside?\": yes (Scan) — changed (recommended: no (Skip))\n")),
             qPrintable(text));
}

void TestGrillMeForm::reportsQuestionsToClarify()
{
    const Spec spec = grillSpec();
    QVector<Answer> answers{
        answer("Q1", QStringLiteral("a")),
        answer("Q2", QJsonArray{QStringLiteral("radio")}, "", "what is a slider here?"),
        answer("Q3", QStringLiteral("change")), // flagged: no change text needed
        answer("Q4", false),
        answer("Q5", QStringLiteral("free text")),
        answer("Q6", 30.0),
    };
    answers[1].clarify = true;
    answers[2].clarify = true;
    const QString text = formatAnswers(spec, answers);
    const QString expected = QStringLiteral(
        "[grill_me answers]\n"
        "Form: Round 2\n"
        "Q1 \"Architecture\": a (Elicitation) — changed (recommended: b2 (No-op tool))\n"
        "Q2 \"Controls\": needs clarification\n"
        "  note: what is a slider here?\n"
        "Q3 \"Module layout\": needs clarification\n"
        "Q4 \"Scan inside?\": no (Skip) (recommended)\n"
        "Q5 \"Describe it\": \"free text\"\n"
        "Q6 \"Days\": 30 (recommended)\n"
        "To clarify: Q2, Q3. Explain them in the user's language, then call grill_me ask again with only these "
        "questions, keeping their ids. The other answers are final.\n");
    QCOMPARE(text, expected);

    // Flagged questions are neither answered nor changed.
    const QString summary = answerSummary(spec, answers);
    QCOMPARE(summary, QStringLiteral("Answered 4 of 6 questions · 1 changed · 2 to clarify"));
    QCOMPARE(summaryFromAnswersText(text), summary);
    const QStringList overview = answerOverview(text);
    QCOMPARE(overview.size(), 6); // the trailing instruction is not a question
    QCOMPARE(overview.at(1), QStringLiteral("Q2 Controls → needs clarification"));

    // The re-asked form keeps the pick and the note, not the flag.
    const QVector<Answer> carried = carryOver(spec, answers);
    QCOMPARE(carried.at(1).value.toArray(), QJsonArray{QStringLiteral("radio")});
    QCOMPARE(carried.at(1).note, QStringLiteral("what is a slider here?"));
    QVERIFY(!carried.at(1).clarify);

    // Nothing flagged: no instruction line.
    QVERIFY(!formatAnswers(spec, defaultAnswers(spec)).contains(QLatin1String("To clarify:")));
}

void TestGrillMeForm::carriesAnswersOverById()
{
    const Spec first = grillSpec();
    QVector<Answer> previous = defaultAnswers(first);
    previous[0].value = QStringLiteral("a");
    previous[1].value = QJsonArray{QStringLiteral("textarea"), QStringLiteral("gone")};

    // Revised round: Q1 kept, Q2 lost an option, Q9 is new.
    const auto next = parseToolInput(obj(R"({"questions":[
        {"id":"Q9","title":"New","type":"text","recommended":"x"},
        {"id":"Q2","title":"Controls","type":"multi","options":["radio","textarea"]},
        {"id":"Q1","title":"Architecture","type":"single",
         "options":[{"value":"a"},{"value":"b2"}],"recommended":"b2"}]})"));
    const QVector<Answer> carried = carryOver(*next, previous);
    QCOMPARE(carried.size(), 3);
    QCOMPARE(carried.at(0).value.toString(), QStringLiteral("x"));        // new: recommendation
    QCOMPARE(carried.at(1).value.toArray(), QJsonArray{QStringLiteral("textarea")}); // unknown value dropped
    QCOMPARE(carried.at(2).value.toString(), QStringLiteral("a"));        // user's pick wins
}

void TestGrillMeForm::recognisesGrillMeToolCalls()
{
    const QJsonObject none;
    QVERIFY(isGrillMeToolCall(QStringLiteral("mcp__grill_me__ask"), QString(), none));
    QVERIFY(isGrillMeToolCall(QString(), QStringLiteral("mcp__grill_me__ask"), none));
    QVERIFY(isGrillMeToolCall(QStringLiteral("grill_me_ask"), QString(), none));
    QVERIFY(isGrillMeToolCall(QString(), QStringLiteral("Tool: grill_me/ask"), none));
    QVERIFY(isGrillMeToolCall(QString(), QStringLiteral("grill_me.ask"), none));
    QVERIFY(isGrillMeToolCall(QString(), QStringLiteral("Some title"),
        obj(R"({"server":"grill_me","tool":"ask","arguments":{}})")));

    QVERIFY(!isGrillMeToolCall(QStringLiteral("ask"), QString(), none)); // other server's tool
    QVERIFY(!isGrillMeToolCall(QStringLiteral("mcp__other__ask"), QString(), none));
    QVERIFY(!isGrillMeToolCall(QStringLiteral("mcp__grill_me__ask_v2"), QString(), none));
    QVERIFY(!isGrillMeToolCall(QStringLiteral("Read"), QStringLiteral("Read grill_me/ask.md"), none));
    // The old name is gone, restored sessions included.
    QVERIFY(!isGrillMeToolCall(QStringLiteral("mcp__notepadai__ask_user"), QString(), none));
}

void TestGrillMeForm::unwrapsNestedArguments()
{
    const QJsonObject direct = obj(R"({"questions":[]})");
    QCOMPARE(toolArguments(direct), direct);
    QCOMPARE(toolArguments(obj(R"({"server":"grill_me","tool":"ask","arguments":{"questions":[]}})")),
             direct);
    const QJsonObject stringified{{QStringLiteral("server"), QStringLiteral("grill_me")},
                                  {QStringLiteral("tool"), QStringLiteral("ask")},
                                  {QStringLiteral("arguments"), QStringLiteral("{\"questions\":[]}")}};
    QCOMPARE(toolArguments(stringified), direct);
}

void TestGrillMeForm::recognisesOmpDeviceWrite()
{
    // omp mounts MCP tools as xd:// devices and calls them through its `write`
    // tool: the title is the model's intent, the arguments a JSON string.
    const QString argsJson = QStringLiteral("{\"title\":\"Round 1\",\"questions\":[{\"id\":\"Q1\","
                                            "\"title\":\"What?\",\"type\":\"textarea\"}]}");
    const QJsonObject raw{{QStringLiteral("path"), QStringLiteral("xd://mcp__grill_me_ask")},
                          {QStringLiteral("content"), argsJson}};
    const QString intent = QStringLiteral("Asking grill-me topic for round 1");
    QVERIFY(isGrillMeToolCall(QString(), intent, raw));
    QJsonObject doubleUnderscore = raw;
    doubleUnderscore.insert(QStringLiteral("path"), QStringLiteral("xd://mcp__grill_me__ask"));
    QVERIFY(isGrillMeToolCall(QString(), intent, doubleUnderscore));
    QJsonObject otherDevice = raw;
    otherDevice.insert(QStringLiteral("path"), QStringLiteral("xd://mcp__other_ask"));
    QVERIFY(!isGrillMeToolCall(QString(), intent, otherDevice));
    // Reading the device (its schema/help) is not a call: no `content`.
    const QJsonObject read{{QStringLiteral("path"), QStringLiteral("xd://mcp__grill_me_ask")}};
    QVERIFY(!isGrillMeToolCall(QStringLiteral("read"), QStringLiteral("Read the ask device"), read));
    QJsonObject file = raw;
    file.insert(QStringLiteral("path"), QStringLiteral("grill_me_ask.json"));
    QVERIFY(!isGrillMeToolCall(QStringLiteral("write"), intent, file));

    const QJsonObject args = toolArguments(raw);
    QCOMPARE(args.value(QStringLiteral("title")).toString(), QStringLiteral("Round 1"));
    const auto spec = parseToolInput(args);
    QVERIFY(spec.has_value());
    QCOMPARE(spec->questions.size(), 1);

    // Completion carries the parsed arguments in rawOutput.details.xdev.
    const QJsonObject noContent{{QStringLiteral("path"), QStringLiteral("xd://mcp__grill_me_ask")},
                                {QStringLiteral("content"), QStringLiteral("not json")}};
    const QJsonObject rawOutput{{QStringLiteral("details"), QJsonObject{{QStringLiteral("xdev"), QJsonObject{
        {QStringLiteral("tool"), QStringLiteral("mcp__grill_me_ask")},
        {QStringLiteral("args"), QJsonDocument::fromJson(argsJson.toUtf8()).object()}}}}}};
    QCOMPARE(toolArguments(noContent, rawOutput).value(QStringLiteral("title")).toString(),
             QStringLiteral("Round 1"));
}

void TestGrillMeForm::infersRecommendedFromRecommendation()
{
    // The agent explained its pick but left `recommended` out.
    QString error;
    auto spec = parseToolInput(obj(R"({"questions":[{"id":"Q1","title":"State?","type":"single",
        "options":[{"value":"review","label":"Review the code"},{"value":"redesign","label":"Reopen the design"},
                   {"value":"archive","label":"Archive now"}],
        "recommendation":"Chọn **review**. Code đã có, nên kiểm chứng trước khi archive."}]})"), &error);
    // "archive" also appears in the reason, but only "review" is in bold.
    QVERIFY2(spec.has_value(), qPrintable(error));
    QCOMPARE(spec->questions.at(0).recommended.toString(), QStringLiteral("review"));

    // The pick leads; the reason names another option ("a" is also an article).
    spec = parseToolInput(obj(R"({"questions":[{"id":"Q1","title":"Cache?","type":"single",
        "options":["a","b","c"],
        "recommendation":"b — a separate cache adds a second source of truth, c is too slow."}]})"), &error);
    QVERIFY2(spec.has_value(), qPrintable(error));
    QCOMPARE(spec->questions.at(0).recommended.toString(), QStringLiteral("b"));

    // Multi: the options named in bold.
    spec = parseToolInput(obj(R"({"questions":[{"id":"Q1","title":"Controls","type":"multi",
        "options":["radio","checkbox","textarea"],
        "recommendation":"**radio** and **checkbox**; a textarea is overkill."}]})"), &error);
    QVERIFY2(spec.has_value(), qPrintable(error));
    QCOMPARE(spec->questions.at(0).recommended.toArray(),
             QJsonArray({QStringLiteral("radio"), QStringLiteral("checkbox")}));

    // No way to tell: the error shows the fix and the values to use.
    spec = parseToolInput(obj(R"({"questions":[{"id":"Q1","title":"State?","type":"single",
        "options":["review","archive"],
        "recommendation":"Either review or archive works here."}]})"), &error);
    QVERIFY(!spec.has_value());
    QVERIFY2(error.contains(QLatin1String("\"recommended\": \"review\""))
                 && error.contains(QLatin1String("\"review\", \"archive\"")),
             qPrintable(error));

    spec = parseToolInput(obj(R"({"questions":[{"id":"Q1","title":"Size gate?","type":"single",
        "options":[{"value":"accept","label":"Accept for v1"},{"value":"now","label":"Support big files now"}],
        "recommendation":"Accept for v1. Note it as a follow-up."}]})"), &error);
    QVERIFY2(spec.has_value(), qPrintable(error));
    QCOMPARE(spec->questions.at(0).recommended.toString(), QStringLiteral("accept"));

    // No reasoning, no recommendation: nothing to preselect, still valid.
    spec = parseToolInput(obj(R"({"questions":[{"id":"Q1","title":"Pick","type":"single",
        "options":["a","b"]}]})"), &error);
    QVERIFY(spec.has_value());
    QVERIFY(spec->questions.at(0).recommended.isNull());
}

void TestGrillMeForm::overviewListsEachAnswer()
{
    const Spec spec = grillSpec();
    const QVector<Answer> answers{
        answer("Q1", QStringLiteral("b2")),
        answer("Q2", QJsonArray{QStringLiteral("radio"), QStringLiteral("textarea")}, "", "keep it small"),
        answer("Q3", QStringLiteral("accept")),
        answer("Q4", false),
        answer("Q5", QStringLiteral("line one\nline two")),
        answer("Q6", QJsonValue()),
    };
    const QStringList lines = answerOverview(formatAnswers(spec, answers), 40);
    QCOMPARE(lines, QStringList({
        QStringLiteral("Q1 Architecture → No-op tool"),
        QStringLiteral("Q2 Controls → radio, textarea · changed"),
        QStringLiteral("Q3 Module layout → accepted"),
        QStringLiteral("Q4 Scan inside? → Skip"),
        QStringLiteral("Q5 Describe it → line one"),
        QStringLiteral("Q6 Days → no answer"),
    }));

    // Long answers are cut; dismiss/clarify prompts have no overview.
    const QStringList cut = answerOverview(formatAnswers(spec, {answer("Q5", QString(60, QLatin1Char('x')))}), 20);
    QCOMPARE(cut.at(4), QStringLiteral("Q5 Describe it → ") + QString(19, QLatin1Char('x')) + QStringLiteral("…"));
    QVERIFY(answerOverview(dismissPrompt(QString())).isEmpty());
}

// The form-level note the user adds at the end: after the answers, before the
// clarify instruction, continuation lines indented.
void TestGrillMeForm::formNoteFollowsTheAnswers()
{
    const Spec spec = grillSpec();
    QVector<Answer> answers{
        answer("Q1", QStringLiteral("b2")),
        answer("Q2", QJsonArray{QStringLiteral("radio"), QStringLiteral("checkbox")}),
        answer("Q3", QStringLiteral("accept")),
        answer("Q4", false),
        answer("Q5", QStringLiteral("free text")),
        answer("Q6", 30.0),
    };
    answers[2].clarify = true;
    const QString note = QStringLiteral("  also: keep the old API\nuntil v3  \n");
    const QString text = formatAnswers(spec, answers, note);
    const QString expected = QStringLiteral(
        "[grill_me answers]\n"
        "Form: Round 2\n"
        "Q1 \"Architecture\": b2 (No-op tool) (recommended)\n"
        "Q2 \"Controls\": radio, checkbox (recommended)\n"
        "Q3 \"Module layout\": needs clarification\n"
        "Q4 \"Scan inside?\": no (Skip) (recommended)\n"
        "Q5 \"Describe it\": \"free text\"\n"
        "Q6 \"Days\": 30 (recommended)\n"
        "Note from the user: also: keep the old API\n"
        "  until v3\n"
        "To clarify: Q3. Explain them in the user's language, then call grill_me ask again with only these "
        "questions, keeping their ids. The other answers are final.\n");
    QCOMPARE(text, expected);

    // A blank note changes nothing.
    QCOMPARE(formatAnswers(spec, answers, QStringLiteral(" \n ")), formatAnswers(spec, answers));

    // The note is not a question, in the live summary and the restored one.
    const QString summary = answerSummary(spec, answers, note);
    QCOMPARE(summary, QStringLiteral("Answered 5 of 6 questions · 0 changed · 1 to clarify · note"));
    QCOMPARE(summaryFromAnswersText(text), summary);
    QCOMPARE(answerSummary(spec, answers, QStringLiteral(" ")),
             QStringLiteral("Answered 5 of 6 questions · 0 changed · 1 to clarify"));

    // The overview ends with the note on one line, cut like the answers.
    const QStringList overview = answerOverview(text, 29);
    QCOMPARE(overview.size(), 7);
    QCOMPARE(overview.at(2), QStringLiteral("Q3 Module layout → needs clarification"));
    QCOMPARE(overview.last(), QStringLiteral("Note → also: keep the old API until…"));

    // A note that looks like an answer line is still the note.
    const QString tricky = formatAnswers(spec, answers, QStringLiteral("Q9 \"Fake\": yes"));
    QCOMPARE(summaryFromAnswersText(tricky), summary);
    QCOMPARE(answerOverview(tricky).last(), QStringLiteral("Note → Q9 \"Fake\": yes"));

    // Dismiss carries the note the same way.
    const QString dismissed = dismissPrompt(note);
    QCOMPARE(dismissed, QStringLiteral("[grill_me answers]\nThe user dismissed the form without answering.\n"
                                       "Note from the user: also: keep the old API\n  until v3"));
    QCOMPARE(summaryFromAnswersText(dismissed), QStringLiteral("Dismissed the form · note"));
    QCOMPARE(answerOverview(dismissed), QStringList{QStringLiteral("Note → also: keep the old API until v3")});
}

// Question-round commands get the grill_me hint after a blank line; the rest
// goes out as typed.
void TestGrillMeForm::hintsQuestionRoundCommands()
{
    const QString hint = withQuestionRoundHint(QStringLiteral("/grill-me")).section(QStringLiteral("\n\n"), 1);
    QVERIFY2(hint.startsWith(QLatin1String("[NotepadAI note, not part of the request]"))
                 && hint.contains(QLatin1String("`ask` tool of the `grill_me` MCP server")),
             qPrintable(hint));

    // Bare command: a space keeps the name apart (Claude Code splits on " ").
    QCOMPARE(withQuestionRoundHint(QStringLiteral("/grill-me")), QStringLiteral("/grill-me \n\n") + hint);
    QCOMPARE(withQuestionRoundHint(QStringLiteral("/grill-with-docs rename it  \n")),
             QStringLiteral("/grill-with-docs rename it\n\n") + hint);
    QCOMPARE(withQuestionRoundHint(QStringLiteral("/brainstorm\nnew login flow")),
             QStringLiteral("/brainstorm\nnew login flow\n\n") + hint);
    QCOMPARE(withQuestionRoundHint(QStringLiteral("/superpowers:brainstorming x")),
             QStringLiteral("/superpowers:brainstorming x\n\n") + hint);
    QCOMPARE(withQuestionRoundHint(QStringLiteral("/Interview-User")), QStringLiteral("/Interview-User \n\n") + hint);
    QCOMPARE(withQuestionRoundHint(QStringLiteral("/plan the cache")), QStringLiteral("/plan the cache\n\n") + hint);
    QCOMPARE(withQuestionRoundHint(QStringLiteral("/planning")), QStringLiteral("/planning \n\n") + hint);

    // A bare /plan may be the agent's own mode switch: the hint would be its task.
    QCOMPARE(withQuestionRoundHint(QStringLiteral("/plan")), QStringLiteral("/plan"));
    QCOMPARE(withQuestionRoundHint(QStringLiteral("/plan  ")), QStringLiteral("/plan  "));
    QCOMPARE(withQuestionRoundHint(QStringLiteral("/kit:plan")), QStringLiteral("/kit:plan \n\n") + hint);

    for (const char *plain : {"grill me on this", "/compact", "/goal plan it", "/spec x", "/", "", " /grill-me"}) {
        const QString text = QString::fromUtf8(plain);
        QCOMPARE(withQuestionRoundHint(text), text);
    }
}

QTEST_GUILESS_MAIN(TestGrillMeForm)
#include "test_grill_me_form.moc"
