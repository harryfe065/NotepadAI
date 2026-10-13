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

#include "AskUserForm.h"

using namespace AskUserForm;

namespace {

QJsonObject obj(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

} // namespace

class TestAskUserForm : public QObject
{
    Q_OBJECT

private slots:
    void parsesGrillRound();
    void rejectsInvalidInputWithReason();
    void acceptsLenientShapes();
    void formatsAnswersForTheAgent();
    void summaryLine();
    void reportsQuestionsToClarify();
    void carriesAnswersOverById();
    void mapsClaudeAskUserQuestion();
    void mapsCodexRequestUserInput();
    void mapsPrimitiveFields();
    void recognisesAskUserToolCalls();
    void unwrapsNestedArguments();
    void recognisesOmpDeviceWrite();
    void infersRecommendedFromRecommendation();
    void overviewListsEachAnswer();
};

void TestAskUserForm::parsesGrillRound()
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
    const auto spec = parseAskUserInput(args, &error);
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

void TestAskUserForm::rejectsInvalidInputWithReason()
{
    QString error;
    QVERIFY(!parseAskUserInput(obj(R"({"questions":[{"id":"Q7","title":"Pick","type":"single"}]})"), &error));
    QVERIFY2(error.contains(QLatin1String("Q7")) && error.contains(QLatin1String("options")), qPrintable(error));

    QVERIFY(!parseAskUserInput(obj(R"({"questions":[]})"), &error));
    QVERIFY(!parseAskUserInput(obj(R"({"questions":[{"id":"A","title":"x","type":"slider"}]})"), &error));
    QVERIFY(error.contains(QLatin1String("slider")));
    QVERIFY(!parseAskUserInput(obj(R"({"questions":[{"id":"A","title":"x","type":"single",
        "options":["a","b"],"recommended":"c"}]})"), &error));
    QVERIFY(error.contains(QLatin1String("\"c\"")));
    QVERIFY(!parseAskUserInput(obj(R"({"questions":[{"id":"A","title":"x","type":"text"},
        {"id":"A","title":"y","type":"text"}]})"), &error));
    QVERIFY(error.contains(QLatin1String("duplicate")));

    QJsonArray many;
    for (int i = 0; i < kMaxQuestions + 1; ++i)
        many.append(QJsonObject{{QStringLiteral("title"), QStringLiteral("t")}, {QStringLiteral("type"), QStringLiteral("text")}});
    QVERIFY(!parseAskUserInput(QJsonObject{{QStringLiteral("questions"), many}}, &error));
}

void TestAskUserForm::acceptsLenientShapes()
{
    // questions as a JSON string, no ids, options as bare strings, lone string
    // for multi, numeric string for number, "yes" for confirm.
    const QJsonObject args{{QStringLiteral("questions"), QStringLiteral(R"([
        {"title":"Pick","type":"multi","options":["x","y"],"recommended":"y"},
        {"title":"Count","type":"number","recommended":"12"},
        {"title":"Ok?","type":"confirm","recommended":"yes"}])")}};
    QString error;
    const auto spec = parseAskUserInput(args, &error);
    QVERIFY2(spec.has_value(), qPrintable(error));
    QCOMPARE(spec->questions.at(0).id, QStringLiteral("Q1"));
    QCOMPARE(spec->questions.at(2).id, QStringLiteral("Q3"));
    QCOMPARE(spec->questions.at(0).recommended.toArray(), QJsonArray{QStringLiteral("y")});
    QCOMPARE(spec->questions.at(1).recommended.toDouble(), 12.0);
    QCOMPARE(spec->questions.at(2).recommended, QJsonValue(true));
}

namespace {

Spec grillSpec()
{
    const auto spec = parseAskUserInput(obj(R"({"title":"Round 2","questions":[
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

void TestAskUserForm::formatsAnswersForTheAgent()
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
        "[ask_user answers]\n"
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

void TestAskUserForm::summaryLine()
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

    // Dismiss and legacy clarify prompts share the collapsed bubble.
    QCOMPARE(summaryFromAnswersText(dismissPrompt(QString())), QStringLiteral("Dismissed the form"));
    const QString legacyClarify = QStringLiteral(
        "[ask_user clarify]\nThe user does not understand question Q2 \"Controls\". Explain it in the "
        "user's language (the language the user writes in), then call ask_user again with the revised form.");
    QCOMPARE(summaryFromAnswersText(legacyClarify), QStringLiteral("Asked to clarify Q2 \"Controls\""));
}

void TestAskUserForm::reportsQuestionsToClarify()
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
        "[ask_user answers]\n"
        "Form: Round 2\n"
        "Q1 \"Architecture\": a (Elicitation) — changed (recommended: b2 (No-op tool))\n"
        "Q2 \"Controls\": needs clarification\n"
        "  note: what is a slider here?\n"
        "Q3 \"Module layout\": needs clarification\n"
        "Q4 \"Scan inside?\": no (Skip) (recommended)\n"
        "Q5 \"Describe it\": \"free text\"\n"
        "Q6 \"Days\": 30 (recommended)\n"
        "To clarify: Q2, Q3. Explain them in the user's language, then call ask_user again with only these "
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

void TestAskUserForm::carriesAnswersOverById()
{
    const Spec first = grillSpec();
    QVector<Answer> previous = defaultAnswers(first);
    previous[0].value = QStringLiteral("a");
    previous[1].value = QJsonArray{QStringLiteral("textarea"), QStringLiteral("gone")};

    // Revised round: Q1 kept, Q2 lost an option, Q9 is new.
    const auto next = parseAskUserInput(obj(R"({"questions":[
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

void TestAskUserForm::mapsClaudeAskUserQuestion()
{
    // Shape produced by claude-agent-acp for two AskUserQuestion questions.
    const QJsonObject schema = obj(R"({"type":"object","properties":{
        "question_0":{"type":"string","title":"Scope","description":"What to delete?",
            "oneOf":[{"const":"node","title":"node","description":"node_modules"},
                     {"const":"rust","title":"rust"}]},
        "question_0_custom":{"type":"string","title":"Other","description":"Free text"},
        "question_1":{"type":"array","title":"Keys","description":"Which keys?",
            "items":{"anyOf":[{"const":"a","title":"a"},{"const":"s","title":"s"}]}},
        "question_1_custom":{"type":"string","title":"Other"}}})");
    const Spec spec = fromElicitation(QStringLiteral("Please answer the following questions."), schema);
    QCOMPARE(spec.source, Source::Elicitation);
    QCOMPARE(spec.title, QStringLiteral("Please answer the following questions."));
    QCOMPARE(spec.questions.size(), 2); // _custom fields merged, not questions

    const Question &q0 = spec.questions.at(0);
    QCOMPARE(q0.fieldKey, QStringLiteral("question_0"));
    QCOMPARE(q0.title, QStringLiteral("Scope"));
    QCOMPARE(q0.body, QStringLiteral("What to delete?"));
    QCOMPARE(q0.type, QuestionType::Single);
    QCOMPARE(q0.options.at(0).description, QStringLiteral("node_modules"));
    QVERIFY(q0.allowOther);
    QCOMPARE(q0.otherFieldKey, QStringLiteral("question_0_custom"));
    QCOMPARE(spec.questions.at(1).type, QuestionType::Multi);

    const QVector<Answer> answers{
        Answer{q0.id, QStringLiteral("rust"), QStringLiteral("only release"), QString()},
        Answer{spec.questions.at(1).id, QJsonArray{QStringLiteral("s")}, QString(), QString()},
    };
    const QJsonObject content = elicitationContent(spec, answers);
    QCOMPARE(content.value(QStringLiteral("question_0")).toString(), QStringLiteral("rust"));
    QCOMPARE(content.value(QStringLiteral("question_0_custom")).toString(), QStringLiteral("only release"));
    QCOMPARE(content.value(QStringLiteral("question_1")).toArray(), QJsonArray{QStringLiteral("s")});
    QVERIFY(!content.contains(QStringLiteral("question_1_custom"))); // empty Other is omitted
}

void TestAskUserForm::mapsCodexRequestUserInput()
{
    // codex-acp: `required` lists every question in the model's order; `_note`
    // companions carry the free answer.
    const QJsonObject schema = obj(R"({"type":"object","required":["zeta","alpha"],"properties":{
        "alpha":{"type":"string","title":"Second?","oneOf":[{"const":"y","title":"Yes"}]},
        "alpha_note":{"type":"string","title":"Additional answer or note"},
        "zeta":{"type":"string","title":"First?","oneOf":[{"const":"n","title":"No"}]}}})");
    const Spec spec = fromElicitation(QStringLiteral("Codex needs your input to continue."), schema);
    QCOMPARE(spec.questions.size(), 2);
    QCOMPARE(spec.questions.at(0).fieldKey, QStringLiteral("zeta"));
    QCOMPARE(spec.questions.at(1).fieldKey, QStringLiteral("alpha"));
    QVERIFY(spec.questions.at(0).required);
    QCOMPARE(spec.questions.at(1).noteFieldKey, QStringLiteral("alpha_note"));
    QVERIFY(!spec.questions.at(1).allowOther);

    const QJsonObject content = elicitationContent(spec, {
        Answer{spec.questions.at(0).id, QStringLiteral("n"), QString(), QString()},
        Answer{spec.questions.at(1).id, QStringLiteral("y"), QString(), QStringLiteral("but later")}});
    QCOMPARE(content.value(QStringLiteral("alpha_note")).toString(), QStringLiteral("but later"));

    // Clarify keeps the required value and asks in the note field.
    Answer unclear{spec.questions.at(1).id, QStringLiteral("y"), QString(), QStringLiteral("which branch?")};
    unclear.clarify = true;
    const QJsonObject asked = elicitationContent(spec, {unclear});
    QCOMPARE(asked.value(QStringLiteral("alpha")).toString(), QStringLiteral("y"));
    const QString note = asked.value(QStringLiteral("alpha_note")).toString();
    QVERIFY2(note.startsWith(QLatin1String("Needs clarification: ")), qPrintable(note));
    QVERIFY(note.endsWith(QLatin1String(" Note: which branch?")));
}

void TestAskUserForm::mapsPrimitiveFields()
{
    const QJsonObject schema = obj(R"({"type":"object","properties":{
        "f10":{"type":"string","maxLength":2000,"title":"Long"},
        "f2":{"type":"boolean","title":"Agree","default":true},
        "f3":{"type":"integer","minimum":1,"maximum":9,"default":3},
        "f4":{"type":"string","enum":["x","y"],"default":"y"},
        "f5":{"type":"string","_meta":{"notepadai/multiline":true}}}})");
    const Spec spec = fromElicitation(QString(), schema);
    QCOMPARE(spec.questions.size(), 5);
    // Natural order: f2 < f3 < f4 < f5 < f10.
    QCOMPARE(spec.questions.at(0).type, QuestionType::Confirm);
    QCOMPARE(spec.questions.at(0).recommended, QJsonValue(true));
    QCOMPARE(spec.questions.at(1).type, QuestionType::Number);
    QVERIFY(spec.questions.at(1).integer);
    QCOMPARE(spec.questions.at(1).max.value_or(0), 9.0);
    QCOMPARE(spec.questions.at(2).type, QuestionType::Single);
    QCOMPARE(spec.questions.at(2).recommended.toString(), QStringLiteral("y"));
    QCOMPARE(spec.questions.at(2).title, QStringLiteral("f4")); // key when untitled
    QCOMPARE(spec.questions.at(3).type, QuestionType::Textarea);
    QCOMPARE(spec.questions.at(4).type, QuestionType::Textarea);

    const QJsonObject content = elicitationContent(spec, {
        Answer{spec.questions.at(0).id, false, QString(), QString()},
        Answer{spec.questions.at(1).id, 4.0, QString(), QString()},
        Answer{spec.questions.at(2).id, QJsonValue(), QString(), QString()}});
    QCOMPARE(content.value(QStringLiteral("f2")), QJsonValue(false));
    QVERIFY(content.value(QStringLiteral("f3")).isDouble());
    QCOMPARE(content.value(QStringLiteral("f3")).toInt(), 4);
    QVERIFY(!content.contains(QStringLiteral("f4"))); // unanswered is omitted
}

void TestAskUserForm::recognisesAskUserToolCalls()
{
    const QJsonObject none;
    QVERIFY(isAskUserToolCall(QStringLiteral("mcp__notepadai__ask_user"), QString(), none));
    QVERIFY(isAskUserToolCall(QString(), QStringLiteral("mcp__notepadai__ask_user"), none));
    QVERIFY(isAskUserToolCall(QStringLiteral("notepadai_ask_user"), QString(), none));
    QVERIFY(isAskUserToolCall(QString(), QStringLiteral("Tool: notepadai/ask_user"), none));
    QVERIFY(isAskUserToolCall(QString(), QStringLiteral("notepadai.ask_user"), none));
    QVERIFY(isAskUserToolCall(QString(), QStringLiteral("Some title"),
        obj(R"({"server":"notepadai","tool":"ask_user","arguments":{}})")));

    QVERIFY(!isAskUserToolCall(QStringLiteral("ask_user"), QString(), none)); // other server's tool
    QVERIFY(!isAskUserToolCall(QStringLiteral("mcp__other__ask_user"), QString(), none));
    QVERIFY(!isAskUserToolCall(QStringLiteral("mcp__notepadai__ask_user_v2"), QString(), none));
    QVERIFY(!isAskUserToolCall(QStringLiteral("Read"), QStringLiteral("Read notepadai/ask_user.md"), none));
}

void TestAskUserForm::unwrapsNestedArguments()
{
    const QJsonObject direct = obj(R"({"questions":[]})");
    QCOMPARE(askUserArguments(direct), direct);
    QCOMPARE(askUserArguments(obj(R"({"server":"notepadai","tool":"ask_user","arguments":{"questions":[]}})")),
             direct);
    const QJsonObject stringified{{QStringLiteral("server"), QStringLiteral("notepadai")},
                                  {QStringLiteral("tool"), QStringLiteral("ask_user")},
                                  {QStringLiteral("arguments"), QStringLiteral("{\"questions\":[]}")}};
    QCOMPARE(askUserArguments(stringified), direct);
}

void TestAskUserForm::recognisesOmpDeviceWrite()
{
    // omp mounts MCP tools as xd:// devices and calls them through its `write`
    // tool: the title is the model's intent, the arguments a JSON string.
    const QString argsJson = QStringLiteral("{\"title\":\"Round 1\",\"questions\":[{\"id\":\"Q1\","
                                            "\"title\":\"What?\",\"type\":\"textarea\"}]}");
    const QJsonObject raw{{QStringLiteral("path"), QStringLiteral("xd://mcp__notepadai_ask_user")},
                          {QStringLiteral("content"), argsJson}};
    const QString intent = QStringLiteral("Asking grill-me topic for round 1");
    QVERIFY(isAskUserToolCall(QString(), intent, raw));
    QJsonObject doubleUnderscore = raw;
    doubleUnderscore.insert(QStringLiteral("path"), QStringLiteral("xd://mcp__notepadai__ask_user"));
    QVERIFY(isAskUserToolCall(QString(), intent, doubleUnderscore));
    QJsonObject otherDevice = raw;
    otherDevice.insert(QStringLiteral("path"), QStringLiteral("xd://mcp__other_ask_user"));
    QVERIFY(!isAskUserToolCall(QString(), intent, otherDevice));
    // Reading the device (its schema/help) is not a call: no `content`.
    const QJsonObject read{{QStringLiteral("path"), QStringLiteral("xd://mcp__notepadai_ask_user")}};
    QVERIFY(!isAskUserToolCall(QStringLiteral("read"), QStringLiteral("Read the ask_user device"), read));
    QJsonObject file = raw;
    file.insert(QStringLiteral("path"), QStringLiteral("notepadai_ask_user.json"));
    QVERIFY(!isAskUserToolCall(QStringLiteral("write"), intent, file));

    const QJsonObject args = askUserArguments(raw);
    QCOMPARE(args.value(QStringLiteral("title")).toString(), QStringLiteral("Round 1"));
    const auto spec = parseAskUserInput(args);
    QVERIFY(spec.has_value());
    QCOMPARE(spec->questions.size(), 1);

    // Completion carries the parsed arguments in rawOutput.details.xdev.
    const QJsonObject noContent{{QStringLiteral("path"), QStringLiteral("xd://mcp__notepadai_ask_user")},
                                {QStringLiteral("content"), QStringLiteral("not json")}};
    const QJsonObject rawOutput{{QStringLiteral("details"), QJsonObject{{QStringLiteral("xdev"), QJsonObject{
        {QStringLiteral("tool"), QStringLiteral("mcp__notepadai_ask_user")},
        {QStringLiteral("args"), QJsonDocument::fromJson(argsJson.toUtf8()).object()}}}}}};
    QCOMPARE(askUserArguments(noContent, rawOutput).value(QStringLiteral("title")).toString(),
             QStringLiteral("Round 1"));
}

void TestAskUserForm::infersRecommendedFromRecommendation()
{
    // The agent explained its pick but left `recommended` out.
    QString error;
    auto spec = parseAskUserInput(obj(R"({"questions":[{"id":"Q1","title":"State?","type":"single",
        "options":[{"value":"review","label":"Review the code"},{"value":"redesign","label":"Reopen the design"},
                   {"value":"archive","label":"Archive now"}],
        "recommendation":"Chọn **review**. Code đã có, nên kiểm chứng trước khi archive."}]})"), &error);
    // "archive" also appears in the reason, but "review" is the pick: ambiguous.
    QVERIFY(!spec.has_value());
    QVERIFY2(error.contains(QLatin1String("recommended")), qPrintable(error));

    spec = parseAskUserInput(obj(R"({"questions":[{"id":"Q1","title":"Size gate?","type":"single",
        "options":[{"value":"accept","label":"Accept for v1"},{"value":"now","label":"Support big files now"}],
        "recommendation":"Accept for v1. Note it as a follow-up."}]})"), &error);
    QVERIFY2(spec.has_value(), qPrintable(error));
    QCOMPARE(spec->questions.at(0).recommended.toString(), QStringLiteral("accept"));

    // No reasoning, no recommendation: nothing to preselect, still valid.
    spec = parseAskUserInput(obj(R"({"questions":[{"id":"Q1","title":"Pick","type":"single",
        "options":["a","b"]}]})"), &error);
    QVERIFY(spec.has_value());
    QVERIFY(spec->questions.at(0).recommended.isNull());
}

void TestAskUserForm::overviewListsEachAnswer()
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

QTEST_GUILESS_MAIN(TestAskUserForm)
#include "test_ask_user_form.moc"
