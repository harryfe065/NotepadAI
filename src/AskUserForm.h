/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef ASK_USER_FORM_H
#define ASK_USER_FORM_H

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <optional>

// Interactive question forms shown in the chat. Two sources share one model:
// the `ask_user` MCP tool (rawInput of its tool call) and ACP form elicitation
// (`elicitation/create` requestedSchema). Pure data + functions, no widgets.
namespace AskUserForm {

inline constexpr char kServerName[] = "notepadai";
inline constexpr char kToolName[] = "ask_user";
inline constexpr char kAnswersHeader[] = "[ask_user answers]";
// Legacy: the one-question clarify prompt of earlier builds (restored history).
inline constexpr char kClarifyHeader[] = "[ask_user clarify]";
inline constexpr char kDismissedLine[] = "The user dismissed the form without answering.";
// AcpMessage::marker / AcpPromptQueue::Item::marker values.
inline constexpr char kMarkerAnswers[] = "ask-user-answers";
inline constexpr char kMarkerNoHuman[] = "ask-user-no-human";

inline constexpr int kMaxQuestions = 30;
inline constexpr int kMaxOptions = 50;
// A single-choice question with more options renders as a dropdown.
inline constexpr int kDropdownThreshold = 7;

enum class QuestionType : std::uint8_t {
    Single,
    Multi,
    Select,
    Proposal,
    Confirm,
    Text,
    Textarea,
    Number,
};

struct Option
{
    QString value;
    QString label;
    QString description;
};

struct Question
{
    QString id;
    QString title;
    QString body; // markdown
    QuestionType type = QuestionType::Single;
    QVector<Option> options;
    // Preselected answer: string (single/select/text/textarea, "accept"/"change"
    // for proposal), string array (multi), bool (confirm), double (number).
    // Undefined = nothing preselected.
    QJsonValue recommended;
    QString recommendation; // markdown, the "why"
    QString yesLabel;
    QString noLabel;
    bool allowOther = false;
    bool required = false;
    QString placeholder;
    std::optional<double> min;
    std::optional<double> max;
    bool integer = false; // number: elicitation `integer`

    // Elicitation only: the content keys this question answers into.
    QString fieldKey;
    QString otherFieldKey; // `<key>_custom` (Claude AskUserQuestion)
    QString noteFieldKey;  // `<key>_note` (Codex request_user_input)

    // Rendered as a dropdown (select, or single with many options).
    bool isDropdown() const;
    bool isChoice() const;
};

enum class Source : std::uint8_t { AskUser, Elicitation };

struct Spec
{
    Source source = Source::AskUser;
    QString title;
    QVector<Question> questions;
};

struct Answer
{
    QString questionId;
    // Same shapes as Question::recommended. Undefined/null = no answer.
    QJsonValue value;
    QString other; // free text in the Other box; for proposal "change": the requested change
    QString note;
    // The user does not understand the question: the agent explains it and
    // asks again. The value is not reported (elicitation still sends it).
    bool clarify = false;
};

// --- ask_user ---------------------------------------------------------------

// Validate and normalise ask_user arguments. Lenient where harmless: missing
// id -> "Q<n>", `questions` given as a JSON string, a lone string for multi,
// numeric strings for number, "yes"/"no" for confirm. On failure returns
// nullopt and sets `error` to a one-line reason naming the question.
std::optional<Spec> parseAskUserInput(const QJsonObject &args, QString *error = nullptr);

// Unwrap a tool call's rawInput into the tool arguments. Some adapters nest
// them as {server, tool, arguments} with arguments possibly a JSON string;
// omp sends {path: "xd://mcp__notepadai_ask_user", content: "<JSON>"} and
// echoes the parsed arguments in rawOutput.details.xdev.args on completion.
QJsonObject askUserArguments(const QJsonObject &rawInput, const QJsonObject &rawOutput = {});

// True when the tool call is notepadai's ask_user, whatever prefix the agent
// adds (mcp__notepadai__ask_user, notepadai_ask_user, notepadai/ask_user, ...),
// including omp's device write to xd://mcp__notepadai_ask_user.
bool isAskUserToolCall(const QString &name, const QString &title, const QJsonObject &rawInput);

// MCP tools/list entry (name, description, inputSchema).
QJsonObject toolDefinition();

// --- elicitation ------------------------------------------------------------

// Map an elicitation/create form request to a Spec. `message` becomes the
// title. Companion fields `<k>_custom` / `<k>_note` merge into question `k`.
Spec fromElicitation(const QString &message, const QJsonObject &requestedSchema);

// `accept` content for an elicitation form.
QJsonObject elicitationContent(const Spec &spec, const QVector<Answer> &answers);

// --- answers ----------------------------------------------------------------

bool hasAnswer(const Question &q, const Answer &a);
// The answer differs from a defined recommendation.
bool isChanged(const Question &q, const Answer &a);

// The prompt sent to the agent after Submit. `note`: the form-level note the
// user adds below the questions, sent as "Note from the user: …" after the
// answers (omitted when blank).
QString formatAnswers(const Spec &spec, const QVector<Answer> &answers, const QString &note = {});
// One line for the collapsed bubble: "Answered 10 questions · 3 changed",
// plus " · 2 to clarify" when the user flagged questions and " · note" when
// the form note is not blank.
QString answerSummary(const Spec &spec, const QVector<Answer> &answers, const QString &note = {});
// Same summary, recomputed from the prompt text (restored history).
QString summaryFromAnswersText(const QString &text);
// One line per question from the prompt text: "Q1 Title → answer[ · changed]",
// then "Note → text" for the form note; answers and the note cut to
// `maxAnswerChars`. Legacy clarify prompts have none; a dismiss prompt has
// only its note line, if any.
QStringList answerOverview(const QString &text, int maxAnswerChars = 90);

QString dismissPrompt(const QString &note);
// Unattended session. `goalAgent`: a goal agent will answer plain-text questions.
QString noHumanPrompt(bool goalAgent);

// Answers prefilled from an earlier form for questions with the same id.
QVector<Answer> carryOver(const Spec &next, const QVector<Answer> &previous);
// Initial answers: the recommendations.
QVector<Answer> defaultAnswers(const Spec &spec);

} // namespace AskUserForm

#endif // ASK_USER_FORM_H
