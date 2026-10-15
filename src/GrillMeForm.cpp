/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "GrillMeForm.h"

#include <QJsonDocument>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <iterator>

namespace GrillMeForm {

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

// The option `pick` names: its value, else (agents mix them up) its value or
// label ignoring case and surrounding spaces. -1 when none does.
int pickedOption(const Question &q, const QString &pick)
{
    if (const int exact = optionIndex(q, pick); exact >= 0)
        return exact;
    const QString loose = pick.trimmed();
    for (int i = 0; i < q.options.size(); ++i) {
        const Option &opt = q.options.at(i);
        if (opt.value.compare(loose, Qt::CaseInsensitive) == 0 || opt.label.compare(loose, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

// `"a", "b", "c"`: the values a `recommended` may name, for error messages.
QString optionValuesText(const Question &q)
{
    QStringList values;
    values.reserve(q.options.size());
    for (const Option &opt : q.options)
        values.append(QLatin1Char('"') + opt.value + QLatin1Char('"'));
    return values.join(QStringLiteral(", "));
}

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

// Normalise `recommended` to the type's shape. False = invalid value.
bool normaliseRecommended(Question &q, const QJsonValue &raw, QString *error)
{
    const QString where = QStringLiteral("question \"%1\": ").arg(q.id);
    if (isEmptyValue(raw)) {
        if (q.type == QuestionType::Proposal)
            q.recommended = QStringLiteral("accept");
        return true;
    }
    const auto notAnOption = [&](const QString &v) {
        return fail(error, where + QStringLiteral("recommended \"%1\" is not an option value; use one of: %2")
                                       .arg(v, optionValuesText(q)));
    };
    switch (q.type) {
    case QuestionType::Single:
    case QuestionType::Select: {
        const QString v = scalarToString(raw.isArray() ? raw.toArray().first() : raw);
        const int i = pickedOption(q, v);
        if (i < 0)
            return notAnOption(v);
        q.recommended = q.options.at(i).value;
        return true;
    }
    case QuestionType::Multi: {
        QJsonArray picked;
        const QJsonArray items = raw.isArray() ? raw.toArray() : QJsonArray{raw};
        for (const auto &item : items) {
            const QString v = scalarToString(item);
            const int i = pickedOption(q, v);
            if (i < 0)
                return notAnOption(v);
            if (!picked.contains(q.options.at(i).value))
                picked.append(q.options.at(i).value);
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

// Position of the first whole-word `word` in `text`, or -1.
qsizetype wordAt(const QString &text, const QString &word)
{
    if (word.isEmpty())
        return -1;
    qsizetype from = 0;
    while ((from = text.indexOf(word, from, Qt::CaseInsensitive)) >= 0) {
        const qsizetype end = from + word.size();
        const bool startOk = from == 0 || !text.at(from - 1).isLetterOrNumber();
        const bool endOk = end >= text.size() || !text.at(end).isLetterOrNumber();
        if (startOk && endOk)
            return from;
        from = end;
    }
    return -1;
}

// First position where `text` names the option (value, or a label of 3+
// characters), or -1.
qsizetype optionMentionAt(const QString &text, const Option &opt)
{
    const qsizetype byValue = wordAt(text, opt.value);
    const qsizetype byLabel = opt.label.size() >= 3 ? wordAt(text, opt.label) : -1;
    if (byValue < 0 || byLabel < 0)
        return std::max(byValue, byLabel);
    return std::min(byValue, byLabel);
}

// Markdown bold spans (`**x**`, `__x__`) joined by newlines: where agents put
// the pick ("Chọn **review**, vì ...").
QString boldSpans(const QString &text)
{
    QString out;
    for (const QLatin1String marker : {QLatin1String("**"), QLatin1String("__")}) {
        qsizetype from = 0;
        while ((from = text.indexOf(marker, from)) >= 0) {
            const qsizetype end = text.indexOf(marker, from + 2);
            if (end < 0)
                break;
            out += text.mid(from + 2, end - from - 2) + QLatin1Char('\n');
            from = end + 2;
        }
    }
    return out;
}

// The option the recommendation text picks, or -1. In order: the only option
// it names; the only one it names in bold; the only one named in its first
// clause (up to . : ; — or a line break), since the pick usually leads.
int mentionedOption(const Question &q)
{
    const QString &text = q.recommendation;
    qsizetype clauseEnd = text.size();
    static constexpr char16_t kClauseBreaks[] = {u'.', u':', u';', u'—', u'\n'};
    for (const char16_t c : kClauseBreaks) {
        const qsizetype at = text.indexOf(c);
        if (at >= 0)
            clauseEnd = std::min(clauseEnd, at);
    }
    const QString bold = boldSpans(text);
    int named = -1;
    int inBold = -1;
    int leading = -1;
    int namedCount = 0;
    int boldCount = 0;
    int leadingCount = 0;
    for (int i = 0; i < q.options.size(); ++i) {
        const qsizetype at = optionMentionAt(text, q.options.at(i));
        if (at < 0)
            continue;
        named = i;
        ++namedCount;
        if (optionMentionAt(bold, q.options.at(i)) >= 0) {
            inBold = i;
            ++boldCount;
        }
        if (at < clauseEnd) {
            leading = i;
            ++leadingCount;
        }
    }
    if (namedCount == 1)
        return named;
    if (boldCount == 1)
        return inBold;
    if (leadingCount == 1)
        return leading;
    return -1;
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

    QJsonArray flagged; // options carrying `"recommended": true`
    if (q.isChoice()) {
        const QJsonArray opts = o.value(QStringLiteral("options")).toArray();
        if (opts.isEmpty())
            return fail(error, QStringLiteral("question \"%1\": type %2 needs a non-empty options array").arg(q.id, typeName));
        if (opts.size() > kMaxOptions)
            return fail(error, QStringLiteral("question \"%1\": at most %2 options").arg(q.id).arg(kMaxOptions));
        QSet<QString> seen;
        for (const auto &ov : opts) {
            Option opt;
            if (ov.isObject()) {
                const QJsonObject oo = ov.toObject();
                opt.value = scalarToString(oo.value(QStringLiteral("value"))).trimmed();
                opt.label = oo.value(QStringLiteral("label")).toString().trimmed();
                opt.description = oo.value(QStringLiteral("description")).toString();
                if (opt.value.isEmpty())
                    opt.value = opt.label;
                // Some agents flag the pick on the option itself.
                if (oo.value(QStringLiteral("recommended")).toBool(false))
                    flagged.append(opt.value.isEmpty() ? opt.label : opt.value);
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
    // The pick under the names agents use for it, then flags on the options.
    QJsonValue recommended = o.value(QStringLiteral("recommended"));
    for (const char *alias : {"default", "suggested", "recommend", "preselected"}) {
        if (!isEmptyValue(recommended))
            break;
        recommended = o.value(QLatin1String(alias));
    }
    if (isEmptyValue(recommended) && !flagged.isEmpty())
        recommended = q.type == QuestionType::Multi ? QJsonValue(flagged) : flagged.first();
    if (!normaliseRecommended(q, recommended, error))
        return false;
    // A choice question whose reasoning names a pick must preselect it: take
    // the option the reasoning picks, else ask the agent to say which.
    if (q.isChoice() && isEmptyValue(q.recommended) && !q.recommendation.trimmed().isEmpty()) {
        if (q.type == QuestionType::Multi) {
            QJsonArray picked;
            const QString bold = boldSpans(q.recommendation);
            for (const Option &opt : std::as_const(q.options)) {
                if (optionMentionAt(bold, opt) >= 0)
                    picked.append(opt.value);
            }
            if (!picked.isEmpty())
                q.recommended = picked;
        } else if (const int pick = mentionedOption(q); pick >= 0) {
            q.recommended = q.options.at(pick).value;
        }
        if (isEmptyValue(q.recommended)) {
            const QString example = q.type == QuestionType::Multi
                ? QStringLiteral("[\"%1\"]").arg(q.options.first().value)
                : QStringLiteral("\"%1\"").arg(q.options.first().value);
            return fail(error, QStringLiteral("question \"%1\": `recommendation` explains a pick but "
                                              "`recommended` is missing. Add it to this question, e.g. "
                                              "\"recommended\": %2, using one of the option values: %3")
                                   .arg(q.id, example, optionValuesText(q)));
        }
    }
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

std::optional<Spec> parseToolInput(const QJsonObject &args, QString *error)
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

// "<anything><sep>grill_me<sep>ask", sep one of the joiners agents use.
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
    // Shortest tail: "grill_me_ask".
    if (s.size() < qsizetype(sizeof(kServerName) + sizeof(kToolName) - 1))
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
// rawInput = {path: "xd://mcp__grill_me_ask", content: "<args JSON>"}.
// A `read` of the same path (the device's schema) has no content: not a call.
bool isGrillMeDevice(const QJsonObject &rawInput)
{
    if (!rawInput.contains(QStringLiteral("content")))
        return false;
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

QJsonObject toolArguments(const QJsonObject &rawInput, const QJsonObject &rawOutput)
{
    if (rawInput.contains(QStringLiteral("questions")))
        return rawInput;
    if (isGrillMeDevice(rawInput)) {
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
            {QStringLiteral("recommendation"), str("Markdown: why you recommend it, shown under the question. "
                                                   "The pick itself goes in `recommended`.")},
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
             "Put a question round to the user as an interactive form (clickable choices and text boxes) shown "
             "in the chat UI. Use it only when a skill, command or instruction tells you to interview the user "
             "in rounds (grill-me, grilling, brainstorming, a spec interview, a plan review): when it says to ask "
             "numbered questions in a text format such as \"❓ **Q1** - ... ➡️ recommended answer\", that format "
             "describes the round's content, not its delivery, so put the round in one call instead of writing "
             "it as text, even for a single question such as what to grill. Outside such rounds do not call it: "
             "decide from the code, the plan and sensible defaults, and ask in plain text only when you are truly "
             "blocked. One entry per question, your suggested answer in `recommended` (always set it on choice "
             "questions: the option value), its reasoning in `recommendation`. One call per round, in the user's "
             "language, then END YOUR TURN: it returns at once without answers, so never call it twice in one "
             "turn and do not repeat the questions as text. Answers arrive as the next user message starting "
             "`[grill_me answers]`; `no answer` = skipped, re-ask later; `needs clarification` = explain and "
             "re-ask only those questions (same ids), the other answers are final. A chat reply replaces the "
             "form. Once the plan is settled or the user said to implement, stop asking: never ask how or "
             "whether to proceed or offer invented next-step menus.")},
        {QStringLiteral("inputSchema"), schema},
    };
}

bool isGrillMeToolCall(const QString &name, const QString &title, const QJsonObject &rawInput)
{
    if (nameMatches(name) || nameMatches(title) || isGrillMeDevice(rawInput))
        return true;
    return rawInput.value(QStringLiteral("server")).toString() == QLatin1String(kServerName)
        && rawInput.value(QStringLiteral("tool")).toString() == QLatin1String(kToolName);
}

namespace {

// Prefixes of command names (namespace stripped) that run question rounds.
constexpr QLatin1String kQuestionRoundCommands[] = {
    QLatin1String("grill"), QLatin1String("plan"), QLatin1String("brainstorm"), QLatin1String("interview"),
};

constexpr QLatin1String kQuestionRoundHint(
    "[NotepadAI note, not part of the request] Deliver each question round of this command through the "
    "`ask` tool of the `grill_me` MCP server (one call per round, then end your turn), never as plain text.");

} // namespace

QString withQuestionRoundHint(const QString &text)
{
    if (!text.startsWith(QLatin1Char('/')))
        return text;
    qsizetype nameEnd = 1;
    while (nameEnd < text.size() && !text.at(nameEnd).isSpace())
        ++nameEnd;
    const QStringView command = QStringView(text).mid(1, nameEnd - 1);
    const QStringView name = command.mid(command.lastIndexOf(QLatin1Char(':')) + 1);
    const bool matches = std::any_of(std::begin(kQuestionRoundCommands), std::end(kQuestionRoundCommands),
                                     [name](QLatin1String prefix) {
                                         return name.startsWith(prefix, Qt::CaseInsensitive);
                                     });
    if (!matches)
        return text;
    const QStringView sent = QStringView(text).trimmed();
    const bool bare = sent.size() == nameEnd;
    // Only the agent's own /plan: a plugin's /x:plan is a skill like any other.
    if (bare && command.compare(QLatin1String("plan"), Qt::CaseInsensitive) == 0)
        return text;
    QString out;
    out.reserve(sent.size() + kQuestionRoundHint.size() + 3);
    out += sent;
    if (bare)
        out += QLatin1Char(' ');
    out += QLatin1String("\n\n");
    out += kQuestionRoundHint;
    return out;
}

namespace {

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
    QString plain = yes ? QStringLiteral("yes") : QStringLiteral("no");
    const QString label = yes ? q.yesLabel : q.noLabel;
    if (label.isEmpty() || label.compare(plain, Qt::CaseInsensitive) == 0)
        return plain;
    // "value (label)", like choices: the overview shows what is in brackets.
    return QStringLiteral("%1 (%2)").arg(plain, label);
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

// The recommended answer as the agent sees the chosen one: choices and
// yes/no with their labels.
QString recommendedText(const Question &q)
{
    const QJsonValue &v = q.recommended;
    switch (q.type) {
    case QuestionType::Single:
    case QuestionType::Select:
        return choiceText(q, v.toString());
    case QuestionType::Multi: {
        QStringList parts;
        for (const auto &item : v.toArray())
            parts.append(choiceText(q, item.toString()));
        return parts.join(QStringLiteral(", "));
    }
    case QuestionType::Proposal:
        return v.toString();
    case QuestionType::Confirm:
        return confirmText(q, v.toBool());
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
            const QJsonArray picked = a.value.toArray();
            for (const auto &item : picked)
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
    const QJsonArray values = v.toArray();
    for (const auto &item : values)
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
        const QJsonArray values = v.toArray();
        for (const auto &item : values) {
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

QString summaryText(int answered, int total, int changed, int clarify, bool hasNote)
{
    const QString head = answered == total
        ? QStringLiteral("Answered %1 question%2").arg(total).arg(total == 1 ? QString() : QStringLiteral("s"))
        : QStringLiteral("Answered %1 of %2 questions").arg(answered).arg(total);
    QString out = head + QStringLiteral(" · %1 changed").arg(changed);
    if (clarify > 0)
        out += QStringLiteral(" · %1 to clarify").arg(clarify);
    if (hasNote)
        out += QStringLiteral(" · note");
    return out;
}

constexpr QLatin1String kNeedsClarification("needs clarification");
// Never a question line: those start `<id> "`.
constexpr QLatin1String kUserNoteLead("Note from the user: ");

// The form note as a prompt line; continuation lines are indented so the
// summary and overview parsers never read them as questions.
QString userNoteLine(const QString &note)
{
    QString text = note.trimmed();
    text.replace(QLatin1Char('\n'), QStringLiteral("\n  "));
    return kUserNoteLead + text;
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

QString formatAnswers(const Spec &spec, const QVector<Answer> &answers, const QString &note)
{
    QString out = QLatin1String(kAnswersHeader) + QLatin1Char('\n');
    if (!spec.title.isEmpty())
        out += QStringLiteral("Form: ") + spec.title + QLatin1Char('\n');
    QStringList unclear;
    for (const Question &q : spec.questions) {
        const Answer *found = findAnswer(answers, q.id);
        Answer a;
        if (found)
            a = *found;
        else
            a.questionId = q.id;
        out += QStringLiteral("%1 %2: ").arg(q.id, quoted(q.title));
        if (a.clarify) {
            out += kNeedsClarification;
            unclear.append(q.id);
        } else if (!hasAnswer(q, a)) {
            out += QStringLiteral("no answer");
        } else {
            out += answerText(q, a);
            if (isChanged(q, a))
                out += QStringLiteral(" — changed (recommended: %1)").arg(recommendedText(q));
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
    if (!note.trimmed().isEmpty())
        out += userNoteLine(note) + QLatin1Char('\n');
    // No `": ` in this line: the summary and overview parsers skip it.
    if (!unclear.isEmpty()) {
        out += QStringLiteral("To clarify: %1. Explain them in the user's language, then call grill_me ask "
                              "again with only these questions, keeping their ids. The other answers are final.\n")
                   .arg(unclear.join(QStringLiteral(", ")));
    }
    return out;
}

QString answerSummary(const Spec &spec, const QVector<Answer> &answers, const QString &note)
{
    int answered = 0;
    int changed = 0;
    int clarify = 0;
    for (const Question &q : spec.questions) {
        const Answer *a = findAnswer(answers, q.id);
        if (a && a->clarify) {
            ++clarify;
            continue;
        }
        if (!a || !hasAnswer(q, *a))
            continue;
        ++answered;
        if (isChanged(q, *a))
            ++changed;
    }
    return summaryText(answered, static_cast<int>(spec.questions.size()), changed, clarify,
                       !note.trimmed().isEmpty());
}

QString summaryFromAnswersText(const QString &text)
{
    if (text.contains(QLatin1String(kDismissedLine))) {
        const bool hasNote = text.contains(QString(QLatin1Char('\n')) + kUserNoteLead);
        return hasNote ? QStringLiteral("Dismissed the form · note") : QStringLiteral("Dismissed the form");
    }
    int total = 0;
    int answered = 0;
    int changed = 0;
    int clarify = 0;
    bool hasNote = false;
    static const QString kClarifyEnd = QStringLiteral("\": ") + kNeedsClarification;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 1; i < lines.size(); ++i) { // line 0 is the header
        const QString &line = lines.at(i);
        if (line.startsWith(kUserNoteLead)) {
            hasNote = true;
            continue;
        }
        if (line.isEmpty() || line.at(0).isSpace() || line.startsWith(QLatin1String("Form: "))
            || !line.contains(QLatin1String("\": "))) {
            continue;
        }
        ++total;
        if (line.endsWith(kClarifyEnd)) {
            ++clarify;
            continue;
        }
        if (line.endsWith(QLatin1String("\": no answer")))
            continue;
        ++answered;
        if (line.contains(QStringLiteral(" — changed (recommended: ")))
            ++changed;
    }
    return summaryText(answered, total, changed, clarify, hasNote);
}

namespace {

// "value (label)" -> "label"; quoted free text -> unquoted.
QString displayItem(QString item)
{
    item = item.trimmed();
    if (item.startsWith(QLatin1String("other: ")))
        return QStringLiteral("other: ") + displayItem(item.mid(7));
    if (item.size() >= 2 && item.startsWith(QLatin1Char('"')) && item.endsWith(QLatin1Char('"')))
        return item.mid(1, item.size() - 2);
    if (item.endsWith(QLatin1Char(')'))) {
        const qsizetype open = item.indexOf(QLatin1String(" ("));
        if (open > 0)
            return item.mid(open + 2, item.size() - open - 3);
    }
    return item;
}

} // namespace

QStringList answerOverview(const QString &text, int maxAnswerChars)
{
    QStringList out;
    if (!text.startsWith(QLatin1String(kAnswersHeader)))
        return out;
    static const QLatin1String kRecommended(" (recommended)");
    static const QString kChanged = QStringLiteral(" — changed (recommended: ");
    auto cut = [maxAnswerChars](QString s) {
        if (maxAnswerChars > 1 && s.size() > maxAnswerChars)
            s = s.left(maxAnswerChars - 1) + QStringLiteral("…");
        return s;
    };
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 1; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (line.startsWith(kUserNoteLead)) {
            // The form note on one line: its indented continuation joins it.
            QString note = line.mid(kUserNoteLead.size());
            while (i + 1 < lines.size() && lines.at(i + 1).startsWith(QLatin1String("  "))) {
                const QString more = lines.at(++i).trimmed();
                if (!more.isEmpty())
                    note += QLatin1Char(' ') + more;
            }
            out.append(QStringLiteral("Note → ") + cut(note));
            continue;
        }
        if (line.isEmpty() || line.at(0).isSpace() || line.startsWith(QLatin1String("Form: ")))
            continue;
        const qsizetype space = line.indexOf(QLatin1Char(' '));
        const qsizetype titleEnd = line.indexOf(QLatin1String("\": "));
        if (space <= 0 || titleEnd < space || line.at(space + 1) != QLatin1Char('"'))
            continue;
        const QString id = line.left(space);
        const QString title = line.mid(space + 2, titleEnd - space - 2);
        QString answer = line.mid(titleEnd + 3);
        bool changed = false;
        if (answer.endsWith(kRecommended)) {
            answer.chop(kRecommended.size());
        } else if (const qsizetype c = answer.indexOf(kChanged); c >= 0) {
            answer.truncate(c);
            changed = true;
        }
        if (answer.endsWith(QLatin1Char('|'))) {
            // Multi-line text: its first line follows, indented.
            const QString next = i + 1 < lines.size() ? lines.at(i + 1).trimmed() : QString();
            answer.chop(1);
            answer += next;
        }
        QString shown;
        if (answer.startsWith(QLatin1Char('"')) || answer.startsWith(QLatin1String("change requested: "))) {
            shown = answer.startsWith(QLatin1Char('"')) ? displayItem(answer) : answer;
        } else {
            QStringList items = answer.split(QStringLiteral(", "));
            for (QString &item : items)
                item = displayItem(item);
            shown = items.join(QStringLiteral(", "));
        }
        shown = cut(shown);
        out.append(QStringLiteral("%1 %2 → %3").arg(id, title, shown)
                   + (changed ? QStringLiteral(" · changed") : QString()));
    }
    return out;
}

QString dismissPrompt(const QString &note)
{
    QString out = QLatin1String(kAnswersHeader) + QLatin1Char('\n') + QLatin1String(kDismissedLine);
    if (!note.trimmed().isEmpty())
        out += QLatin1Char('\n') + userNoteLine(note);
    return out;
}

QString noHumanPrompt(bool goalAgent)
{
    if (goalAgent) {
        return QStringLiteral(
            "A goal agent is driving this session and no human will answer forms. Do not call "
            "grill_me ask. If you need a decision, ask it as plain text in your reply; the goal "
            "agent will answer it.");
    }
    return QStringLiteral(
        "This session runs unattended (scheduled task) and no human will answer forms. Do not "
        "call grill_me ask. If you need a decision, pick the most reasonable option yourself, "
        "say which one you picked and why, and continue.");
}

} // namespace GrillMeForm
