/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "AskUserForm.h"

#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace AskUserForm {

namespace {

struct TypeName
{
    const char *name;
    QuestionType type;
};

constexpr TypeName kTypeNames[] = {
    {"single", QuestionType::Single},     {"multi", QuestionType::Multi},
    {"select", QuestionType::Select},     {"proposal", QuestionType::Proposal},
    {"confirm", QuestionType::Confirm},   {"text", QuestionType::Text},
    {"textarea", QuestionType::Textarea}, {"number", QuestionType::Number},
};

std::optional<QuestionType> typeFromName(const QString &name)
{
    for (const TypeName &t : kTypeNames) {
        if (name.compare(QLatin1String(t.name), Qt::CaseInsensitive) == 0)
            return t.type;
    }
    return std::nullopt;
}

QString scalarToString(const QJsonValue &v)
{
    if (v.isString())
        return v.toString();
    if (v.isDouble())
        return QString::number(v.toDouble(), 'g', 15);
    if (v.isBool())
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return {};
}

std::optional<double> numberOf(const QJsonValue &v)
{
    if (v.isDouble())
        return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().trimmed().toDouble(&ok);
        if (ok)
            return d;
    }
    return std::nullopt;
}

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

int optionIndex(const Question &q, const QString &value)
{
    for (int i = 0; i < q.options.size(); ++i) {
        if (q.options.at(i).value == value)
            return i;
    }
    return -1;
}

// Normalise `recommended` to the type's shape. False = invalid value.
bool normaliseRecommended(Question &q, const QJsonValue &raw, QString *error)
{
    const QString where = QStringLiteral("question \"%1\": ").arg(q.id);
    if (raw.isUndefined() || raw.isNull()) {
        if (q.type == QuestionType::Proposal)
            q.recommended = QStringLiteral("accept");
        return true;
    }
    switch (q.type) {
    case QuestionType::Single:
    case QuestionType::Select: {
        const QString v = scalarToString(raw);
        if (optionIndex(q, v) < 0)
            return fail(error, where + QStringLiteral("recommended \"%1\" is not one of the option values").arg(v));
        q.recommended = v;
        return true;
    }
    case QuestionType::Multi: {
        QJsonArray picked;
        const QJsonArray items = raw.isArray() ? raw.toArray() : QJsonArray{raw};
        for (const QJsonValue &item : items) {
            const QString v = scalarToString(item);
            if (optionIndex(q, v) < 0)
                return fail(error, where + QStringLiteral("recommended \"%1\" is not one of the option values").arg(v));
            if (!picked.contains(v))
                picked.append(v);
        }
        q.recommended = picked;
        return true;
    }
    case QuestionType::Proposal: {
        const QString v = scalarToString(raw).trimmed().toLower();
        if (v != QLatin1String("accept") && v != QLatin1String("change"))
            return fail(error, where + QStringLiteral("recommended must be \"accept\" or \"change\""));
        q.recommended = v;
        return true;
    }
    case QuestionType::Confirm: {
        if (raw.isBool()) {
            q.recommended = raw.toBool();
            return true;
        }
        const QString v = scalarToString(raw).trimmed().toLower();
        if (v == QLatin1String("yes") || v == QLatin1String("true")
            || (!q.yesLabel.isEmpty() && v == q.yesLabel.toLower())) {
            q.recommended = true;
            return true;
        }
        if (v == QLatin1String("no") || v == QLatin1String("false")
            || (!q.noLabel.isEmpty() && v == q.noLabel.toLower())) {
            q.recommended = false;
            return true;
        }
        return fail(error, where + QStringLiteral("recommended must be true or false"));
    }
    case QuestionType::Text:
    case QuestionType::Textarea:
        q.recommended = scalarToString(raw);
        return true;
    case QuestionType::Number: {
        const auto d = numberOf(raw);
        if (!d)
            return fail(error, where + QStringLiteral("recommended must be a number"));
        q.recommended = *d;
        return true;
    }
    }
    return true;
}

bool parseQuestion(const QJsonValue &value, int index, Question *out, QString *error)
{
    const QString position = QStringLiteral("question %1").arg(index + 1);
    if (!value.isObject())
        return fail(error, position + QStringLiteral(" is not an object"));
    const QJsonObject o = value.toObject();

    Question q;
    q.id = scalarToString(o.value(QStringLiteral("id"))).trimmed();
    if (q.id.isEmpty())
        q.id = QStringLiteral("Q%1").arg(index + 1);
    q.body = o.value(QStringLiteral("body")).toString();
    q.title = o.value(QStringLiteral("title")).toString().trimmed();
    if (q.title.isEmpty())
        q.title = q.body.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (q.title.isEmpty())
        return fail(error, QStringLiteral("question \"%1\": title is missing").arg(q.id));

    const QString typeName = o.value(QStringLiteral("type")).toString().trimmed();
    const auto type = typeFromName(typeName.isEmpty() ? QStringLiteral("single") : typeName);
    if (!type) {
        return fail(error, QStringLiteral("question \"%1\": unknown type \"%2\" (use single, multi, select, "
                                          "proposal, confirm, text, textarea or number)")
                               .arg(q.id, typeName));
    }
    q.type = *type;
    q.recommendation = o.value(QStringLiteral("recommendation")).toString();
    q.yesLabel = o.value(QStringLiteral("yesLabel")).toString();
    q.noLabel = o.value(QStringLiteral("noLabel")).toString();
    q.placeholder = o.value(QStringLiteral("placeholder")).toString();
    q.required = o.value(QStringLiteral("required")).toBool(false);
    if (const auto mn = numberOf(o.value(QStringLiteral("min"))))
        q.min = *mn;
    if (const auto mx = numberOf(o.value(QStringLiteral("max"))))
        q.max = *mx;

    if (q.isChoice()) {
        const QJsonArray opts = o.value(QStringLiteral("options")).toArray();
        if (opts.isEmpty())
            return fail(error, QStringLiteral("question \"%1\": type %2 needs a non-empty options array").arg(q.id, typeName));
        if (opts.size() > kMaxOptions)
            return fail(error, QStringLiteral("question \"%1\": at most %2 options").arg(q.id).arg(kMaxOptions));
        QSet<QString> seen;
        for (const QJsonValue &ov : opts) {
            Option opt;
            if (ov.isObject()) {
                const QJsonObject oo = ov.toObject();
                opt.value = scalarToString(oo.value(QStringLiteral("value"))).trimmed();
                opt.label = oo.value(QStringLiteral("label")).toString().trimmed();
                opt.description = oo.value(QStringLiteral("description")).toString();
                if (opt.value.isEmpty())
                    opt.value = opt.label;
            } else {
                opt.value = scalarToString(ov).trimmed();
            }
            if (opt.label.isEmpty())
                opt.label = opt.value;
            if (opt.value.isEmpty())
                return fail(error, QStringLiteral("question \"%1\": every option needs a value").arg(q.id));
            if (seen.contains(opt.value))
                return fail(error, QStringLiteral("question \"%1\": duplicate option value \"%2\"").arg(q.id, opt.value));
            seen.insert(opt.value);
            q.options.append(opt);
        }
        q.allowOther = o.value(QStringLiteral("allowOther")).toBool(true);
    }

    if (q.min && q.max && *q.min > *q.max)
        return fail(error, QStringLiteral("question \"%1\": min is greater than max").arg(q.id));
    if (!normaliseRecommended(q, o.value(QStringLiteral("recommended")), error))
        return false;
    *out = std::move(q);
    return true;
}

} // namespace

bool Question::isDropdown() const
{
    return type == QuestionType::Select
        || (type == QuestionType::Single && options.size() > kDropdownThreshold);
}

bool Question::isChoice() const
{
    return type == QuestionType::Single || type == QuestionType::Multi || type == QuestionType::Select;
}

std::optional<Spec> parseAskUserInput(const QJsonObject &args, QString *error)
{
    QJsonValue questionsValue = args.value(QStringLiteral("questions"));
    if (questionsValue.isString()) {
        // Some models stringify nested arrays.
        const QJsonDocument doc = QJsonDocument::fromJson(questionsValue.toString().toUtf8());
        if (doc.isArray())
            questionsValue = doc.array();
    }
    if (!questionsValue.isArray()) {
        fail(error, QStringLiteral("\"questions\" must be an array of question objects"));
        return std::nullopt;
    }
    const QJsonArray questions = questionsValue.toArray();
    if (questions.isEmpty()) {
        fail(error, QStringLiteral("\"questions\" is empty"));
        return std::nullopt;
    }
    if (questions.size() > kMaxQuestions) {
        fail(error, QStringLiteral("at most %1 questions per call; split the round").arg(kMaxQuestions));
        return std::nullopt;
    }

    Spec spec;
    spec.source = Source::AskUser;
    spec.title = args.value(QStringLiteral("title")).toString().trimmed();
    spec.questions.reserve(questions.size());
    QSet<QString> ids;
    for (int i = 0; i < questions.size(); ++i) {
        Question q;
        if (!parseQuestion(questions.at(i), i, &q, error))
            return std::nullopt;
        if (ids.contains(q.id)) {
            fail(error, QStringLiteral("duplicate question id \"%1\"").arg(q.id));
            return std::nullopt;
        }
        ids.insert(q.id);
        spec.questions.append(std::move(q));
    }
    return spec;
}

namespace {

// "<anything><sep>notepadai<sep>ask_user", sep one of the joiners agents use.
bool nameMatches(const QString &raw)
{
    static const QLatin1String kJoiners[] = {
        QLatin1String("__"), QLatin1String("_"), QLatin1String("/"),
        QLatin1String("."), QLatin1String(": "), QLatin1String(":"),
    };
    static const QStringList kTails = [] {
        QStringList tails;
        for (const QLatin1String &joiner : kJoiners)
            tails.append(QLatin1String(kServerName) + joiner + QLatin1String(kToolName));
        return tails;
    }();
    const QStringView s = QStringView(raw).trimmed();
    if (s.size() < 18) // strlen("notepadai_ask_user")
        return false;
    for (const QString &tail : kTails) {
        if (!s.endsWith(tail, Qt::CaseInsensitive))
            continue;
        const qsizetype before = s.size() - tail.size() - 1;
        if (before < 0 || !s.at(before).isLetterOrNumber())
            return true;
    }
    return false;
}

constexpr QLatin1String kDevicePrefix("xd://");

// omp mounts MCP tools as devices and calls them through its `write` tool:
// rawInput = {path: "xd://mcp__notepadai_ask_user", content: "<args JSON>"}.
bool isAskUserDevice(const QJsonObject &rawInput)
{
    const QString path = rawInput.value(QStringLiteral("path")).toString().trimmed();
    return path.startsWith(kDevicePrefix, Qt::CaseInsensitive) && nameMatches(path.mid(kDevicePrefix.size()));
}

QJsonObject objectFrom(const QJsonValue &v)
{
    if (v.isObject())
        return v.toObject();
    if (v.isString()) {
        const QJsonDocument doc = QJsonDocument::fromJson(v.toString().toUtf8());
        if (doc.isObject())
            return doc.object();
    }
    return {};
}

} // namespace

QJsonObject askUserArguments(const QJsonObject &rawInput, const QJsonObject &rawOutput)
{
    if (rawInput.contains(QStringLiteral("questions")))
        return rawInput;
    if (isAskUserDevice(rawInput)) {
        QJsonObject args = objectFrom(rawInput.value(QStringLiteral("content")));
        if (args.isEmpty()) {
            // Completion: omp echoes the parsed arguments.
            args = rawOutput.value(QStringLiteral("details")).toObject()
                       .value(QStringLiteral("xdev")).toObject()
                       .value(QStringLiteral("args")).toObject();
        }
        return args;
    }
    if (rawInput.contains(QStringLiteral("tool"))) {
        const QJsonObject args = objectFrom(rawInput.value(QStringLiteral("arguments")));
        if (!args.isEmpty())
            return args;
    }
    return rawInput;
}

QJsonObject toolDefinition()
{
    auto str = [](const char *description) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                           {QStringLiteral("description"), QString::fromUtf8(description)}};
    };
    const QJsonObject option{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("value"), str("Machine value, e.g. \"a\" or \"b2\"; returned in the answer.")},
            {QStringLiteral("label"), str("Short label shown to the user. Defaults to value.")},
            {QStringLiteral("description"), str("Optional markdown: what this option means, pros and cons.")},
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("value")}},
    };
    const QJsonObject question{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("id"), str("Stable id shown as the number, e.g. \"Q1\" or \"Q14'\". Reuse the same "
                                       "id when you re-ask or revise a question so earlier answers carry over.")},
            {QStringLiteral("title"), str("Short question title.")},
            {QStringLiteral("body"), str("Optional markdown: context, the choices explained, code blocks, lists.")},
            {QStringLiteral("type"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("single"), QStringLiteral("multi"), QStringLiteral("select"),
                    QStringLiteral("proposal"), QStringLiteral("confirm"), QStringLiteral("text"),
                    QStringLiteral("textarea"), QStringLiteral("number")}},
                {QStringLiteral("description"), QStringLiteral(
                     "single: pick one of options (radio; dropdown above 7 options). "
                     "multi: pick any of options (checkboxes; also for \"combine (a) and (c)\"). "
                     "select: dropdown. "
                     "proposal: a proposal in body for the user to Accept or Change (Change requires text); "
                     "use it for \"do exactly as above\" questions. "
                     "confirm: yes/no; word it so yes accepts your recommendation. "
                     "text: one line. textarea: open question, several lines. number: a number.")},
            }},
            {QStringLiteral("options"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("items"), option},
                {QStringLiteral("description"), QStringLiteral(
                     "Required for single, multi and select. Do not add an \"Other\" option: "
                     "a free-text Other box is provided.")},
            }},
            {QStringLiteral("recommended"), QJsonObject{
                {QStringLiteral("anyOf"), QJsonArray{
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}},
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}},
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}},
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}},
                }},
                {QStringLiteral("description"), QStringLiteral(
                     "Your recommended answer, preselected: an option value (single/select), an array of "
                     "values (multi), \"accept\" or \"change\" (proposal, default accept), true/false "
                     "(confirm), a string (text/textarea) or a number.")},
            }},
            {QStringLiteral("recommendation"), str("Markdown: your recommendation and why, shown under the "
                                                   "question.")},
            {QStringLiteral("yesLabel"), str("confirm: label for yes.")},
            {QStringLiteral("noLabel"), str("confirm: label for no.")},
            {QStringLiteral("allowOther"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")},
                {QStringLiteral("description"), QStringLiteral("Show the Other box (default true).")},
            }},
            {QStringLiteral("required"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")},
                {QStringLiteral("description"), QStringLiteral(
                     "Block submit until answered (default false: unanswered questions arrive as "
                     "\"no answer\").")},
            }},
            {QStringLiteral("placeholder"), str("Hint text for text, textarea and number.")},
            {QStringLiteral("min"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
            {QStringLiteral("max"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("id"), QStringLiteral("title"),
                                                QStringLiteral("type")}},
    };
    const QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("title"), str("Optional form title, e.g. \"Round 3\".")},
            {QStringLiteral("questions"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("items"), question},
                {QStringLiteral("minItems"), 1},
                {QStringLiteral("maxItems"), kMaxQuestions},
            }},
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("questions")}},
    };
    return QJsonObject{
        {QStringLiteral("name"), QLatin1String(kToolName)},
        {QStringLiteral("description"), QStringLiteral(
             "Use this tool whenever you need a decision or input from the user before you can continue: "
             "interviews, grilling (grill-me), clarifying questions, choices between options, plan reviews. "
             "This replaces any plain-text question format a skill or instruction prescribes: do not write the "
             "questions as text, and do not repeat them in your reply after calling. Map each numbered question "
             "onto one entry in `questions`, put your suggested answer in `recommended`, and put the reasoning "
             "in `recommendation`. Ask one whole round per call (any number of questions, unlike "
             "AskUserQuestion's 4; prefer this tool over AskUserQuestion and request_user_input), write in the "
             "user's language, then END YOUR TURN: the tool shows an interactive form in the NotepadAI chat and "
             "returns immediately without answers. The answers arrive as the next user message beginning with "
             "`[ask_user answers]`; `no answer` means unanswered, so re-ask it in a later round. A message "
             "beginning with `[ask_user clarify]` asks you to explain one question in the user's language and "
             "call ask_user again with the same ids. Never call it twice in one turn.")},
        {QStringLiteral("inputSchema"), schema},
    };
}

bool isAskUserToolCall(const QString &name, const QString &title, const QJsonObject &rawInput)
{
    if (nameMatches(name) || nameMatches(title) || isAskUserDevice(rawInput))
        return true;
    return rawInput.value(QStringLiteral("server")).toString() == QLatin1String(kServerName)
        && rawInput.value(QStringLiteral("tool")).toString() == QLatin1String(kToolName);
}

namespace {

bool isEmptyValue(const QJsonValue &v)
{
    if (v.isUndefined() || v.isNull())
        return true;
    if (v.isString())
        return v.toString().trimmed().isEmpty();
    if (v.isArray())
        return v.toArray().isEmpty();
    return false;
}

QString formatNumber(double d)
{
    return QString::number(d, 'g', 15);
}

QString choiceText(const Question &q, const QString &value)
{
    const int i = optionIndex(q, value);
    if (i < 0)
        return value;
    const Option &opt = q.options.at(i);
    if (opt.label.isEmpty() || opt.label == opt.value)
        return opt.value;
    return QStringLiteral("%1 (%2)").arg(opt.value, opt.label);
}

QString confirmText(const Question &q, bool yes)
{
    const QString plain = yes ? QStringLiteral("yes") : QStringLiteral("no");
    const QString label = yes ? q.yesLabel : q.noLabel;
    if (label.isEmpty() || label.compare(plain, Qt::CaseInsensitive) == 0)
        return plain;
    return QStringLiteral("%1 (%2)").arg(label, plain);
}

QString quoted(const QString &text)
{
    return QLatin1Char('"') + text + QLatin1Char('"');
}

// Multi-line text goes on indented lines below the question line.
QString textBlock(const QString &text)
{
    if (!text.contains(QLatin1Char('\n')))
        return quoted(text);
    QString out = QStringLiteral("|");
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines)
        out += QStringLiteral("\n    ") + line;
    return out;
}

// The recommendation rendered like an answer value.
QString valueText(const Question &q, const QJsonValue &v)
{
    switch (q.type) {
    case QuestionType::Single:
    case QuestionType::Select:
    case QuestionType::Proposal:
        return v.toString();
    case QuestionType::Multi: {
        QStringList parts;
        for (const QJsonValue &item : v.toArray())
            parts.append(item.toString());
        return parts.join(QStringLiteral(", "));
    }
    case QuestionType::Confirm:
        return v.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
    case QuestionType::Number:
        return formatNumber(v.toDouble());
    case QuestionType::Text:
    case QuestionType::Textarea:
        return quoted(v.toString());
    }
    return {};
}

QString answerText(const Question &q, const Answer &a)
{
    const QString other = a.other.trimmed();
    switch (q.type) {
    case QuestionType::Single:
    case QuestionType::Select:
    case QuestionType::Multi: {
        QStringList parts;
        if (q.type == QuestionType::Multi) {
            for (const QJsonValue &item : a.value.toArray())
                parts.append(choiceText(q, item.toString()));
        } else if (!isEmptyValue(a.value)) {
            parts.append(choiceText(q, a.value.toString()));
        }
        if (!other.isEmpty())
            parts.append(QStringLiteral("other: ") + textBlock(other));
        return parts.join(QStringLiteral(", "));
    }
    case QuestionType::Proposal:
        if (a.value.toString() == QLatin1String("change"))
            return QStringLiteral("change requested: ") + textBlock(other);
        return QStringLiteral("accepted");
    case QuestionType::Confirm:
        return confirmText(q, a.value.toBool());
    case QuestionType::Number:
        return formatNumber(a.value.toDouble());
    case QuestionType::Text:
    case QuestionType::Textarea:
        return textBlock(a.value.toString());
    }
    return {};
}

QStringList sortedStrings(const QJsonValue &v)
{
    QStringList items;
    for (const QJsonValue &item : v.toArray())
        items.append(item.toString());
    items.sort();
    return items;
}

const Answer *findAnswer(const QVector<Answer> &answers, const QString &id)
{
    for (const Answer &a : answers) {
        if (a.questionId == id)
            return &a;
    }
    return nullptr;
}

// Drop values the question no longer offers. Undefined when nothing is left.
QJsonValue fitToQuestion(const Question &q, const QJsonValue &v)
{
    if (isEmptyValue(v))
        return {};
    switch (q.type) {
    case QuestionType::Single:
    case QuestionType::Select:
        return optionIndex(q, v.toString()) >= 0 ? v : QJsonValue();
    case QuestionType::Multi: {
        QJsonArray kept;
        for (const QJsonValue &item : v.toArray()) {
            if (optionIndex(q, item.toString()) >= 0)
                kept.append(item);
        }
        return kept.isEmpty() ? QJsonValue() : QJsonValue(kept);
    }
    case QuestionType::Confirm:
        return v.isBool() ? v : QJsonValue();
    case QuestionType::Number:
        return v.isDouble() ? v : QJsonValue();
    case QuestionType::Proposal:
    case QuestionType::Text:
    case QuestionType::Textarea:
        return v.isString() ? v : QJsonValue();
    }
    return {};
}

QString summaryText(int answered, int total, int changed)
{
    const QString head = answered == total
        ? QStringLiteral("Answered %1 question%2").arg(total).arg(total == 1 ? QString() : QStringLiteral("s"))
        : QStringLiteral("Answered %1 of %2 questions").arg(answered).arg(total);
    return head + QStringLiteral(" · %1 changed").arg(changed);
}

} // namespace

bool hasAnswer(const Question &q, const Answer &a)
{
    if (q.isChoice())
        return !isEmptyValue(a.value) || !a.other.trimmed().isEmpty();
    if (q.type == QuestionType::Proposal && a.value.toString() == QLatin1String("change"))
        return !a.other.trimmed().isEmpty();
    return !isEmptyValue(a.value);
}

bool isChanged(const Question &q, const Answer &a)
{
    if (isEmptyValue(q.recommended) || !hasAnswer(q, a))
        return false;
    switch (q.type) {
    case QuestionType::Multi:
        return sortedStrings(a.value) != sortedStrings(q.recommended);
    case QuestionType::Number:
        return a.value.toDouble() != q.recommended.toDouble();
    default:
        return a.value != q.recommended;
    }
}

QVector<Answer> defaultAnswers(const Spec &spec)
{
    QVector<Answer> out;
    out.reserve(spec.questions.size());
    for (const Question &q : spec.questions) {
        Answer a;
        a.questionId = q.id;
        a.value = q.recommended;
        out.append(a);
    }
    return out;
}

QVector<Answer> carryOver(const Spec &next, const QVector<Answer> &previous)
{
    QVector<Answer> out = defaultAnswers(next);
    for (int i = 0; i < next.questions.size(); ++i) {
        const Question &q = next.questions.at(i);
        const Answer *old = findAnswer(previous, q.id);
        if (!old)
            continue;
        const QJsonValue fitted = fitToQuestion(q, old->value);
        if (!fitted.isUndefined())
            out[i].value = fitted;
        if ((q.isChoice() && q.allowOther) || q.type == QuestionType::Proposal)
            out[i].other = old->other;
        out[i].note = old->note;
    }
    return out;
}

QString formatAnswers(const Spec &spec, const QVector<Answer> &answers)
{
    QString out = QLatin1String(kAnswersHeader) + QLatin1Char('\n');
    if (!spec.title.isEmpty())
        out += QStringLiteral("Form: ") + spec.title + QLatin1Char('\n');
    for (const Question &q : spec.questions) {
        const Answer *found = findAnswer(answers, q.id);
        const Answer a = found ? *found : Answer{q.id, {}, {}, {}};
        out += QStringLiteral("%1 %2: ").arg(q.id, quoted(q.title));
        if (!hasAnswer(q, a)) {
            out += QStringLiteral("no answer");
        } else {
            out += answerText(q, a);
            if (isChanged(q, a))
                out += QStringLiteral(" — changed (recommended: %1)").arg(valueText(q, q.recommended));
            else if (!isEmptyValue(q.recommended))
                out += QStringLiteral(" (recommended)");
        }
        out += QLatin1Char('\n');
        const QString note = a.note.trimmed();
        if (!note.isEmpty()) {
            QString indented = note;
            indented.replace(QLatin1Char('\n'), QStringLiteral("\n    "));
            out += QStringLiteral("  note: ") + indented + QLatin1Char('\n');
        }
    }
    return out;
}

QString answerSummary(const Spec &spec, const QVector<Answer> &answers)
{
    int answered = 0;
    int changed = 0;
    for (const Question &q : spec.questions) {
        const Answer *a = findAnswer(answers, q.id);
        if (!a || !hasAnswer(q, *a))
            continue;
        ++answered;
        if (isChanged(q, *a))
            ++changed;
    }
    return summaryText(answered, static_cast<int>(spec.questions.size()), changed);
}

QString summaryFromAnswersText(const QString &text)
{
    if (text.startsWith(QLatin1String(kClarifyHeader))) {
        static const QLatin1String kLead("does not understand question ");
        const qsizetype from = text.indexOf(kLead);
        const qsizetype to = text.indexOf(QLatin1String(". Explain"));
        if (from >= 0 && to > from)
            return QStringLiteral("Asked to clarify ") + text.mid(from + kLead.size(), to - from - kLead.size());
        return QStringLiteral("Asked to clarify a question");
    }
    if (text.contains(QLatin1String(kDismissedLine)))
        return QStringLiteral("Dismissed the form");
    int total = 0;
    int answered = 0;
    int changed = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 1; i < lines.size(); ++i) { // line 0 is the header
        const QString &line = lines.at(i);
        if (line.isEmpty() || line.at(0).isSpace() || line.startsWith(QLatin1String("Form: "))
            || !line.contains(QLatin1String("\": "))) {
            continue;
        }
        ++total;
        if (line.endsWith(QLatin1String("\": no answer")))
            continue;
        ++answered;
        if (line.contains(QStringLiteral(" — changed (recommended: ")))
            ++changed;
    }
    return summaryText(answered, total, changed);
}

namespace {

// "question_2" < "question_10": digit runs compare by value.
bool naturalLess(const QString &a, const QString &b)
{
    int i = 0;
    int j = 0;
    while (i < a.size() && j < b.size()) {
        if (a.at(i).isDigit() && b.at(j).isDigit()) {
            int ei = i;
            int ej = j;
            while (ei < a.size() && a.at(ei).isDigit())
                ++ei;
            while (ej < b.size() && b.at(ej).isDigit())
                ++ej;
            const qulonglong na = a.mid(i, ei - i).toULongLong();
            const qulonglong nb = b.mid(j, ej - j).toULongLong();
            if (na != nb)
                return na < nb;
            i = ei;
            j = ej;
            continue;
        }
        if (a.at(i) != b.at(j))
            return a.at(i) < b.at(j);
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

// Options from `oneOf` / `anyOf` ([{const,title,description}]) or `enum`.
QVector<Option> enumOptions(const QJsonObject &schema)
{
    QVector<Option> out;
    QJsonArray titled = schema.value(QStringLiteral("oneOf")).toArray();
    if (titled.isEmpty())
        titled = schema.value(QStringLiteral("anyOf")).toArray();
    for (const QJsonValue &v : titled) {
        const QJsonObject o = v.toObject();
        Option opt;
        opt.value = scalarToString(o.value(QStringLiteral("const")));
        opt.label = o.value(QStringLiteral("title")).toString();
        opt.description = o.value(QStringLiteral("description")).toString();
        if (opt.label.isEmpty())
            opt.label = opt.value;
        if (!opt.value.isEmpty())
            out.append(opt);
    }
    if (!out.isEmpty())
        return out;
    const QJsonArray plain = schema.value(QStringLiteral("enum")).toArray();
    const QJsonArray names = schema.value(QStringLiteral("enumNames")).toArray();
    for (int i = 0; i < plain.size(); ++i) {
        Option opt;
        opt.value = scalarToString(plain.at(i));
        opt.label = i < names.size() ? names.at(i).toString() : opt.value;
        if (!opt.value.isEmpty())
            out.append(opt);
    }
    return out;
}

constexpr int kTextareaMinLength = 500;

Question questionFromProperty(const QString &key, const QJsonObject &p)
{
    Question q;
    q.id = key;
    q.fieldKey = key;
    q.title = p.value(QStringLiteral("title")).toString().trimmed();
    q.body = p.value(QStringLiteral("description")).toString();
    if (q.title.isEmpty())
        q.title = key;
    const QString type = p.value(QStringLiteral("type")).toString();
    const QJsonValue def = p.value(QStringLiteral("default"));
    if (type == QLatin1String("array")) {
        q.type = QuestionType::Multi;
        q.options = enumOptions(p.value(QStringLiteral("items")).toObject());
        if (def.isArray())
            q.recommended = def;
    } else if (type == QLatin1String("boolean")) {
        q.type = QuestionType::Confirm;
        if (def.isBool())
            q.recommended = def;
    } else if (type == QLatin1String("number") || type == QLatin1String("integer")) {
        q.type = QuestionType::Number;
        q.integer = type == QLatin1String("integer");
        if (const auto mn = numberOf(p.value(QStringLiteral("minimum"))))
            q.min = *mn;
        if (const auto mx = numberOf(p.value(QStringLiteral("maximum"))))
            q.max = *mx;
        if (def.isDouble())
            q.recommended = def;
    } else {
        q.options = enumOptions(p);
        if (!q.options.isEmpty()) {
            q.type = QuestionType::Single;
        } else {
            const bool multiline = p.value(QStringLiteral("_meta")).toObject()
                                       .value(QStringLiteral("notepadai/multiline")).toBool(false)
                || p.value(QStringLiteral("maxLength")).toInt(0) >= kTextareaMinLength;
            q.type = multiline ? QuestionType::Textarea : QuestionType::Text;
        }
        if (def.isString())
            q.recommended = def;
    }
    if (q.isChoice() && !q.recommended.isUndefined())
        q.recommended = fitToQuestion(q, q.recommended);
    return q;
}

} // namespace

Spec fromElicitation(const QString &message, const QJsonObject &requestedSchema)
{
    Spec spec;
    spec.source = Source::Elicitation;
    spec.title = message.trimmed();

    const QJsonObject props = requestedSchema.value(QStringLiteral("properties")).toObject();
    QSet<QString> required;
    QStringList order;
    for (const QJsonValue &v : requestedSchema.value(QStringLiteral("required")).toArray()) {
        const QString key = v.toString();
        if (props.contains(key) && !required.contains(key)) {
            required.insert(key);
            order.append(key);
        }
    }
    QStringList rest;
    for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
        if (!required.contains(it.key()))
            rest.append(it.key());
    }
    std::sort(rest.begin(), rest.end(), naturalLess);
    order += rest;

    // A companion belongs to its base field only when that base exists.
    auto companionBase = [&props](const QString &key, QLatin1String suffix) -> QString {
        if (!key.endsWith(suffix))
            return {};
        const QString base = key.left(key.size() - suffix.size());
        return props.contains(base) ? base : QString();
    };

    QHash<QString, int> indexByKey;
    for (const QString &key : std::as_const(order)) {
        if (!companionBase(key, QLatin1String("_custom")).isEmpty()
            || !companionBase(key, QLatin1String("_note")).isEmpty()) {
            continue;
        }
        Question q = questionFromProperty(key, props.value(key).toObject());
        q.required = required.contains(key);
        indexByKey.insert(key, static_cast<int>(spec.questions.size()));
        spec.questions.append(std::move(q));
    }
    for (const QString &key : std::as_const(order)) {
        const QString customBase = companionBase(key, QLatin1String("_custom"));
        const QString noteBase = companionBase(key, QLatin1String("_note"));
        const QString base = customBase.isEmpty() ? noteBase : customBase;
        const auto it = indexByKey.constFind(base);
        if (it == indexByKey.constEnd())
            continue;
        Question &q = spec.questions[it.value()];
        if (!customBase.isEmpty()) {
            q.otherFieldKey = key;
            q.allowOther = true;
        } else {
            q.noteFieldKey = key;
        }
    }
    return spec;
}

QJsonObject elicitationContent(const Spec &spec, const QVector<Answer> &answers)
{
    QJsonObject content;
    for (const Question &q : spec.questions) {
        const Answer *a = findAnswer(answers, q.id);
        if (!a)
            continue;
        if (!isEmptyValue(a->value)) {
            if (q.type == QuestionType::Number && q.integer)
                content.insert(q.fieldKey, qRound64(a->value.toDouble()));
            else
                content.insert(q.fieldKey, a->value);
        }
        QString other = a->other.trimmed();
        QString note = a->note.trimmed();
        // No dedicated note field: the note rides along in the Other box.
        if (q.noteFieldKey.isEmpty() && !q.otherFieldKey.isEmpty() && !note.isEmpty()) {
            other = other.isEmpty() ? note : other + QStringLiteral("; ") + note;
            note.clear();
        }
        if (!q.otherFieldKey.isEmpty() && !other.isEmpty())
            content.insert(q.otherFieldKey, other);
        if (!q.noteFieldKey.isEmpty()) {
            // Without an Other field, the Other text shares the note field.
            QStringList parts;
            if (q.otherFieldKey.isEmpty() && !other.isEmpty())
                parts.append(other);
            if (!note.isEmpty())
                parts.append(note);
            if (!parts.isEmpty())
                content.insert(q.noteFieldKey, parts.join(QStringLiteral("; ")));
        }
    }
    return content;
}

QString dismissPrompt(const QString &note)
{
    QString out = QLatin1String(kAnswersHeader) + QLatin1Char('\n') + QLatin1String(kDismissedLine);
    if (!note.trimmed().isEmpty())
        out += QStringLiteral("\nNote from the user: ") + note.trimmed();
    return out;
}

QString clarifyPrompt(const Question &q)
{
    return QLatin1String(kClarifyHeader)
        + QStringLiteral("\nThe user does not understand question %1 %2. Explain it in the user's "
                         "language (the language the user writes in), then call ask_user again with "
                         "the revised form. Keep the same question ids so earlier answers carry over.")
              .arg(q.id, quoted(q.title));
}

QString freeformPrompt(const QString &text)
{
    return QLatin1String(kAnswersHeader)
        + QStringLiteral("\nThe user answered in free form instead of using the form:\n") + text;
}

QString noHumanPrompt(bool goalAgent)
{
    if (goalAgent) {
        return QStringLiteral(
            "A goal agent is driving this session and no human will answer forms. Do not call "
            "ask_user or AskUserQuestion. If you need a decision, ask it as plain text in your "
            "reply; the goal agent will answer it.");
    }
    return QStringLiteral(
        "This session runs unattended (scheduled task) and no human will answer forms. Do not "
        "call ask_user or AskUserQuestion. If you need a decision, pick the most reasonable "
        "option yourself, say which one you picked and why, and continue.");
}

} // namespace AskUserForm
