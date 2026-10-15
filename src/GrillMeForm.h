/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef GRILL_ME_FORM_H
#define GRILL_ME_FORM_H

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <optional>

// Question forms shown in the chat, built from the `ask` tool of the
// `grill_me` MCP server (rawInput of its tool call). Pure data + functions,
// no widgets.
namespace GrillMeForm {

inline constexpr char kServerName[] = "grill_me";
inline constexpr char kToolName[] = "ask";
inline constexpr char kAnswersHeader[] = "[grill_me answers]";
inline constexpr char kDismissedLine[] = "The user dismissed the form without answering.";
// AcpMessage::marker / AcpPromptQueue::Item::marker values.
inline constexpr char kMarkerAnswers[] = "grill-me-answers";
inline constexpr char kMarkerNoHuman[] = "grill-me-no-human";

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

    // Rendered as a dropdown (select, or single with many options).
    bool isDropdown() const;
    bool isChoice() const;
};

struct Spec
{
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
    // asks again. The value is not reported.
    bool clarify = false;
};

// --- tool call --------------------------------------------------------------

// Validate and normalise the tool's arguments. Lenient where harmless: missing
// id -> "Q<n>", `questions` given as a JSON string, a lone string for multi,
// numeric strings for number, "yes"/"no" for confirm. On failure returns
// nullopt and sets `error` to a one-line reason naming the question.
std::optional<Spec> parseToolInput(const QJsonObject &args, QString *error = nullptr);

// Unwrap a tool call's rawInput into the tool arguments. Some adapters nest
// them as {server, tool, arguments} with arguments possibly a JSON string;
// omp sends {path: "xd://mcp__grill_me_ask", content: "<JSON>"} and echoes
// the parsed arguments in rawOutput.details.xdev.args on completion.
QJsonObject toolArguments(const QJsonObject &rawInput, const QJsonObject &rawOutput = {});

// True when the tool call is grill_me's ask, whatever prefix the agent adds
// (mcp__grill_me__ask, grill_me_ask, grill_me/ask, grill_me.ask, ...),
// including omp's device write to xd://mcp__grill_me_ask.
bool isGrillMeToolCall(const QString &name, const QString &title, const QJsonObject &rawInput);

// MCP tools/list entry (name, description, inputSchema).
QJsonObject toolDefinition();

// --- question-round commands ------------------------------------------------

// The hint appended to a user prompt that starts a question-round command
// (/grill-…, /plan …, /brainstorm…, /interview…, also namespaced like
// /plugin:brainstorming), so the agent sends its rounds through the tool.
// Returns the text to send: `text` itself when it is no such command, else
// `text` plus a blank line and the hint, with a space after a bare command
// name (Claude Code reads the name up to the first space). A bare `/plan` is
// left alone: as an agent built-in, the hint would become its task.
QString withQuestionRoundHint(const QString &text);

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
// `maxAnswerChars`. A dismiss prompt has only its note line, if any.
QStringList answerOverview(const QString &text, int maxAnswerChars = 90);

QString dismissPrompt(const QString &note);
// Unattended session. `goalAgent`: a goal agent will answer plain-text questions.
QString noHumanPrompt(bool goalAgent);

// Answers prefilled from an earlier form for questions with the same id.
QVector<Answer> carryOver(const Spec &next, const QVector<Answer> &previous);
// Initial answers: the recommendations.
QVector<Answer> defaultAnswers(const Spec &spec);

} // namespace GrillMeForm

#endif // GRILL_ME_FORM_H
