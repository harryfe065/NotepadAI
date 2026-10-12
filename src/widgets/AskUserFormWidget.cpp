/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "AskUserFormWidget.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleValidator>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

using AskUserForm::Answer;
using AskUserForm::Question;
using AskUserForm::QuestionType;

namespace {

constexpr int kOptionIndent = 22; // description under a radio/checkbox label
constexpr int kTextareaLines = 4;
constexpr int kOtherButtonId = 1 << 20; // past any option index; never a Qt auto id
// Dropdown item data of the "Other…" entry; never a valid option value.
const QString &otherItemData()
{
    static const QString kData = QStringLiteral("\u0001other");
    return kData;
}

QString optionText(const AskUserForm::Option &opt, bool recommended)
{
    return recommended ? AskUserFormWidget::tr("%1 (recommended)").arg(opt.label) : opt.label;
}

void fitLines(QPlainTextEdit *edit, int lines)
{
    const QFontMetrics fm(edit->font());
    const int frame = 2 * edit->frameWidth();
    const int margins = qRound(2 * edit->document()->documentMargin());
    edit->setFixedHeight(fm.lineSpacing() * lines + frame + margins);
}

} // namespace

AskUserFormWidget::AskUserFormWidget(AskUserForm::Spec spec, const QVector<Answer> &initial,
                                     QWidget *parent)
    : QFrame(parent)
    , m_spec(std::move(spec))
{
    setObjectName(QStringLiteral("askUserForm"));
    setStyleSheet(QStringLiteral(
        "QFrame#askUserForm { border: 1px solid palette(mid); border-radius: 6px; }"));
    buildUi();
    for (int i = 0; i < m_spec.questions.size(); ++i) {
        const QString &id = m_spec.questions.at(i).id;
        for (const Answer &a : initial) {
            if (a.questionId == id) {
                applyAnswer(i, a);
                break;
            }
        }
    }
}

QLabel *AskUserFormWidget::mutedLabel(const QString &text, Qt::TextFormat format, QWidget *parent) const
{
    auto *label = new QLabel(parent);
    label->setTextFormat(format);
    label->setText(text);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    label->setOpenExternalLinks(true);
    label->setStyleSheet(QStringLiteral("QLabel { color: palette(placeholder-text); }"));
    return label;
}

void AskUserFormWidget::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 8);
    root->setSpacing(6);

    auto *header = new QHBoxLayout;
    header->setSpacing(4);
    m_toggle = new QToolButton(this);
    m_toggle->setAutoRaise(true);
    m_toggle->setCheckable(true);
    m_toggle->setChecked(true);
    m_toggle->setText(QStringLiteral("▾"));
    m_toggle->setToolTip(tr("Show or hide the questions"));
    connect(m_toggle, &QToolButton::toggled, this, &AskUserFormWidget::setExpanded);
    header->addWidget(m_toggle);

    const qsizetype count = m_spec.questions.size();
    QString title = m_spec.title;
    if (title.isEmpty())
        title = count <= 1 ? tr("Question") : tr("%1 questions").arg(count);
    else if (count > 1)
        title = tr("%1 · %2 questions").arg(title).arg(count);
    m_titleLabel = new QLabel(title, this);
    m_titleLabel->setWordWrap(true);
    QFont bold = m_titleLabel->font();
    bold.setWeight(QFont::DemiBold);
    m_titleLabel->setFont(bold);
    header->addWidget(m_titleLabel, 1);
    m_statusLabel = mutedLabel(QString(), Qt::PlainText, this);
    m_statusLabel->setWordWrap(false);
    m_statusLabel->hide();
    header->addWidget(m_statusLabel);
    root->addLayout(header);

    m_body = new QWidget(this);
    auto *bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(8);
    m_rows.resize(m_spec.questions.size());
    for (int i = 0; i < m_spec.questions.size(); ++i)
        bodyLayout->addWidget(buildQuestion(i));
    root->addWidget(m_body);

    m_footer = new QWidget(this);
    auto *footer = new QHBoxLayout(m_footer);
    footer->setContentsMargins(0, 2, 0, 0);
    footer->setSpacing(6);
    footer->addWidget(mutedLabel(tr("Enter to submit · Shift+Enter for a new line"), Qt::PlainText,
                                 m_footer), 1);
    m_dismissButton = new QPushButton(tr("Dismiss"), m_footer);
    m_dismissButton->setAutoDefault(false);
    m_dismissButton->setToolTip(tr("Tell the agent you will not answer this form"));
    connect(m_dismissButton, &QPushButton::clicked, this, [this]() {
        if (isPending())
            emit dismissed();
    });
    footer->addWidget(m_dismissButton);
    m_submitButton = new QPushButton(tr("Submit"), m_footer);
    m_submitButton->setAutoDefault(false);
    connect(m_submitButton, &QPushButton::clicked, this, &AskUserFormWidget::trySubmit);
    footer->addWidget(m_submitButton);
    root->addWidget(m_footer);
}

QWidget *AskUserFormWidget::buildQuestion(int index)
{
    const Question &q = m_spec.questions.at(index);
    Row &row = m_rows[index];
    row.container = new QWidget(m_body);
    auto *layout = new QVBoxLayout(row.container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // Title row: "Q1 · Title", Required, Note, Clarify.
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(4);
    const bool numbered = m_spec.source == AskUserForm::Source::AskUser || m_spec.questions.size() > 1;
    const QString label = m_spec.source == AskUserForm::Source::AskUser ? q.id : QStringLiteral("Q%1").arg(index + 1);
    auto *title = new QLabel(numbered ? QStringLiteral("%1 · %2").arg(label, q.title) : q.title, row.container);
    title->setWordWrap(true);
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont bold = title->font();
    bold.setWeight(QFont::DemiBold);
    title->setFont(bold);
    titleRow->addWidget(title, 1);

    row.requiredLabel = new QLabel(tr("Required"), row.container);
    row.requiredLabel->setVisible(q.required);
    row.requiredLabel->setStyleSheet(QStringLiteral("QLabel { color: palette(placeholder-text); }"));
    titleRow->addWidget(row.requiredLabel);

    row.noteButton = new QToolButton(row.container);
    row.noteButton->setAutoRaise(true);
    row.noteButton->setCheckable(true);
    row.noteButton->setText(tr("Note"));
    row.noteButton->setToolTip(tr("Add a note to this answer"));
    // Elicitation without a free-text field cannot carry a note.
    const bool canNote = m_spec.source == AskUserForm::Source::AskUser
        || !q.noteFieldKey.isEmpty() || !q.otherFieldKey.isEmpty();
    row.noteButton->setVisible(canNote);
    titleRow->addWidget(row.noteButton);

    // Elicitation carries the request in a free-text companion field.
    const bool canClarify = m_spec.source == AskUserForm::Source::AskUser
        || !q.otherFieldKey.isEmpty() || !q.noteFieldKey.isEmpty();
    if (canClarify) {
        auto *clarify = new QToolButton(row.container);
        clarify->setAutoRaise(true);
        clarify->setText(tr("Clarify"));
        clarify->setToolTip(tr("Ask the agent to explain this question"));
        connect(clarify, &QToolButton::clicked, this, [this, index]() {
            if (isPending())
                emit clarifyRequested(index);
        });
        titleRow->addWidget(clarify);
    }
    layout->addLayout(titleRow);

    if (!q.body.trimmed().isEmpty()) {
        auto *body = new QLabel(row.container);
        body->setTextFormat(Qt::MarkdownText);
        body->setText(q.body);
        body->setWordWrap(true);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
        body->setOpenExternalLinks(true);
        layout->addWidget(body);
    }

    switch (q.type) {
    case QuestionType::Single:
    case QuestionType::Select:
        if (q.isDropdown())
            buildDropdown(index, row, layout);
        else
            buildChoice(index, row, layout);
        break;
    case QuestionType::Multi:
        buildChoice(index, row, layout);
        break;
    case QuestionType::Confirm:
    case QuestionType::Proposal: {
        row.group = new QButtonGroup(row.container);
        const bool proposal = q.type == QuestionType::Proposal;
        const QString yes = proposal ? tr("Accept") : (q.yesLabel.isEmpty() ? tr("Yes") : q.yesLabel);
        const QString no = proposal ? tr("Change…") : (q.noLabel.isEmpty() ? tr("No") : q.noLabel);
        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(12);
        for (const QString &text : {yes, no}) {
            auto *rb = new QRadioButton(text, row.container);
            row.group->addButton(rb, static_cast<int>(row.optionButtons.size()));
            row.optionButtons.append(rb);
            buttons->addWidget(rb);
            watch(rb);
        }
        buttons->addStretch(1);
        layout->addLayout(buttons);
        if (proposal) {
            row.textEdit = new QPlainTextEdit(row.container);
            row.textEdit->setPlaceholderText(tr("What should change?"));
            row.textEdit->setTabChangesFocus(true);
            fitLines(row.textEdit, kTextareaLines);
            row.textEdit->hide();
            layout->addWidget(row.textEdit);
            watch(row.textEdit);
            connect(row.group, &QButtonGroup::idToggled, this, [this, index](int, bool) {
                updateProposalEditor(index);
            });
        }
        break;
    }
    case QuestionType::Text:
    case QuestionType::Number: {
        row.lineEdit = new QLineEdit(row.container);
        row.lineEdit->setPlaceholderText(q.placeholder);
        if (q.type == QuestionType::Number) {
            auto *validator = new QDoubleValidator(row.lineEdit);
            // Answers go to the agent with a dot decimal, whatever the UI locale.
            validator->setLocale(QLocale::c());
            validator->setNotation(QDoubleValidator::StandardNotation);
            if (q.min)
                validator->setBottom(*q.min);
            if (q.max)
                validator->setTop(*q.max);
            if (q.integer)
                validator->setDecimals(0);
            row.lineEdit->setValidator(validator);
            if (q.placeholder.isEmpty() && (q.min || q.max)) {
                row.lineEdit->setPlaceholderText(
                    tr("%1 to %2").arg(q.min ? QString::number(*q.min) : QStringLiteral("−∞"),
                                       q.max ? QString::number(*q.max) : QStringLiteral("∞")));
            }
        }
        layout->addWidget(row.lineEdit);
        watch(row.lineEdit);
        break;
    }
    case QuestionType::Textarea:
        row.textEdit = new QPlainTextEdit(row.container);
        row.textEdit->setPlaceholderText(q.placeholder);
        row.textEdit->setTabChangesFocus(true);
        fitLines(row.textEdit, kTextareaLines);
        layout->addWidget(row.textEdit);
        watch(row.textEdit);
        break;
    }

    if (!q.recommendation.trimmed().isEmpty()) {
        layout->addWidget(mutedLabel(tr("Recommended: %1").arg(q.recommendation), Qt::MarkdownText,
                                     row.container));
    }

    row.noteEdit = new QLineEdit(row.container);
    row.noteEdit->setPlaceholderText(tr("Note"));
    row.noteEdit->hide();
    layout->addWidget(row.noteEdit);
    watch(row.noteEdit);
    QLineEdit *noteEdit = row.noteEdit;
    connect(row.noteButton, &QToolButton::toggled, this, [noteEdit](bool on) {
        noteEdit->setVisible(on);
        if (on)
            noteEdit->setFocus();
    });
    return row.container;
}

void AskUserFormWidget::buildChoice(int index, Row &row, QVBoxLayout *layout)
{
    const Question &q = m_spec.questions.at(index);
    const bool multi = q.type == QuestionType::Multi;
    if (!multi)
        row.group = new QButtonGroup(row.container);
    for (int i = 0; i < q.options.size(); ++i) {
        const AskUserForm::Option &opt = q.options.at(i);
        const bool recommended = multi ? q.recommended.toArray().contains(opt.value)
                                       : q.recommended.toString() == opt.value;
        const QString text = optionText(opt, recommended);
        QAbstractButton *button = multi ? static_cast<QAbstractButton *>(new QCheckBox(text, row.container))
                                        : static_cast<QAbstractButton *>(new QRadioButton(text, row.container));
        if (row.group)
            row.group->addButton(button, i);
        row.optionButtons.append(button);
        layout->addWidget(button);
        watch(button);
        if (!opt.description.trimmed().isEmpty()) {
            QLabel *desc = mutedLabel(opt.description, Qt::MarkdownText, row.container);
            desc->setContentsMargins(kOptionIndent, 0, 0, 0);
            layout->addWidget(desc);
        }
    }
    if (q.allowOther)
        buildOther(row, layout, multi);
}

void AskUserFormWidget::buildDropdown(int index, Row &row, QVBoxLayout *layout)
{
    const Question &q = m_spec.questions.at(index);
    row.combo = new QComboBox(row.container);
    row.combo->addItem(tr("Choose…"), QString()); // index 0 = no answer
    for (const AskUserForm::Option &opt : q.options) {
        row.combo->addItem(optionText(opt, q.recommended.toString() == opt.value), opt.value);
        if (!opt.description.isEmpty())
            row.combo->setItemData(row.combo->count() - 1, opt.description, Qt::ToolTipRole);
    }
    if (q.allowOther)
        row.combo->addItem(tr("Other…"), otherItemData());
    layout->addWidget(row.combo);
    watch(row.combo);
    if (q.allowOther) {
        row.otherEdit = new QLineEdit(row.container);
        row.otherEdit->setPlaceholderText(tr("Other"));
        row.otherEdit->hide();
        layout->addWidget(row.otherEdit);
        watch(row.otherEdit);
        QComboBox *combo = row.combo;
        QLineEdit *other = row.otherEdit;
        connect(combo, &QComboBox::currentIndexChanged, this, [combo, other](int i) {
            const bool isOther = combo->itemData(i).toString() == otherItemData();
            other->setVisible(isOther);
            if (isOther)
                other->setFocus();
        });
    }
}

void AskUserFormWidget::buildOther(Row &row, QVBoxLayout *layout, bool checkbox)
{
    auto *line = new QHBoxLayout;
    line->setSpacing(6);
    row.otherButton = checkbox ? static_cast<QAbstractButton *>(new QCheckBox(tr("Other:"), row.container))
                               : static_cast<QAbstractButton *>(new QRadioButton(tr("Other:"), row.container));
    if (row.group)
        row.group->addButton(row.otherButton, kOtherButtonId);
    row.otherEdit = new QLineEdit(row.container);
    row.otherEdit->setPlaceholderText(tr("Type your own answer"));
    line->addWidget(row.otherButton);
    line->addWidget(row.otherEdit, 1);
    layout->addLayout(line);
    watch(row.otherButton);
    watch(row.otherEdit);
    QAbstractButton *button = row.otherButton;
    // Typing an answer selects Other.
    connect(row.otherEdit, &QLineEdit::textEdited, this, [button](const QString &text) {
        if (!text.isEmpty())
            button->setChecked(true);
    });
}

void AskUserFormWidget::watch(QWidget *w)
{
    w->installEventFilter(this);
}

void AskUserFormWidget::updateProposalEditor(int index)
{
    Row &row = m_rows[index];
    if (!row.textEdit || row.optionButtons.size() < 2)
        return;
    const bool change = row.optionButtons.at(1)->isChecked();
    row.textEdit->setVisible(change);
    if (change && isPending())
        row.textEdit->setFocus();
}

void AskUserFormWidget::applyAnswer(int index, const Answer &a)
{
    const Question &q = m_spec.questions.at(index);
    Row &row = m_rows[index];
    switch (q.type) {
    case QuestionType::Single:
    case QuestionType::Select:
    case QuestionType::Multi: {
        if (row.combo) {
            int found = row.combo->findData(a.value.toString());
            if (a.value.toString().isEmpty())
                found = a.other.isEmpty() ? 0 : row.combo->findData(otherItemData());
            row.combo->setCurrentIndex(found < 0 ? 0 : found);
        } else {
            const QJsonArray picked = q.type == QuestionType::Multi ? a.value.toArray()
                                                                     : QJsonArray{a.value};
            for (int i = 0; i < q.options.size(); ++i)
                row.optionButtons.at(i)->setChecked(picked.contains(q.options.at(i).value));
            if (row.otherButton && !a.other.isEmpty()
                && (q.type == QuestionType::Multi || a.value.toString().isEmpty())) {
                row.otherButton->setChecked(true);
            }
        }
        if (row.otherEdit)
            row.otherEdit->setText(a.other);
        break;
    }
    case QuestionType::Confirm:
        if (a.value.isBool())
            row.optionButtons.at(a.value.toBool() ? 0 : 1)->setChecked(true);
        break;
    case QuestionType::Proposal:
        row.optionButtons.at(a.value.toString() == QLatin1String("change") ? 1 : 0)->setChecked(true);
        row.textEdit->setPlainText(a.other);
        updateProposalEditor(index);
        break;
    case QuestionType::Text:
        row.lineEdit->setText(a.value.toString());
        break;
    case QuestionType::Number:
        if (a.value.isDouble())
            row.lineEdit->setText(QString::number(a.value.toDouble(), 'g', 15));
        break;
    case QuestionType::Textarea:
        row.textEdit->setPlainText(a.value.toString());
        break;
    }
    row.noteEdit->setText(a.note);
    if (!a.note.isEmpty())
        row.noteButton->setChecked(true);
}

Answer AskUserFormWidget::answerAt(int index) const
{
    const Question &q = m_spec.questions.at(index);
    const Row &row = m_rows.at(index);
    Answer a;
    a.questionId = q.id;
    a.note = row.noteEdit->text().trimmed();
    switch (q.type) {
    case QuestionType::Single:
    case QuestionType::Select:
        if (row.combo) {
            const QString v = row.combo->currentData().toString();
            if (v == otherItemData())
                a.other = row.otherEdit ? row.otherEdit->text().trimmed() : QString();
            else if (!v.isEmpty())
                a.value = v;
        } else {
            const int id = row.group->checkedId();
            if (id == kOtherButtonId && row.otherEdit)
                a.other = row.otherEdit->text().trimmed();
            else if (id >= 0)
                a.value = q.options.at(id).value;
        }
        break;
    case QuestionType::Multi: {
        QJsonArray picked;
        for (int i = 0; i < q.options.size(); ++i) {
            if (row.optionButtons.at(i)->isChecked())
                picked.append(q.options.at(i).value);
        }
        if (!picked.isEmpty())
            a.value = picked;
        if (row.otherButton && row.otherButton->isChecked())
            a.other = row.otherEdit->text().trimmed();
        break;
    }
    case QuestionType::Confirm:
        if (row.group->checkedId() >= 0)
            a.value = row.group->checkedId() == 0;
        break;
    case QuestionType::Proposal:
        if (row.group->checkedId() == 1) {
            a.value = QStringLiteral("change");
            a.other = row.textEdit->toPlainText().trimmed();
        } else if (row.group->checkedId() == 0) {
            a.value = QStringLiteral("accept");
        }
        break;
    case QuestionType::Text: {
        const QString text = row.lineEdit->text().trimmed();
        if (!text.isEmpty())
            a.value = text;
        break;
    }
    case QuestionType::Number: {
        bool ok = false;
        const double d = QLocale::c().toDouble(row.lineEdit->text().trimmed(), &ok);
        if (ok)
            a.value = d;
        break;
    }
    case QuestionType::Textarea: {
        const QString text = row.textEdit->toPlainText().trimmed();
        if (!text.isEmpty())
            a.value = text;
        break;
    }
    }
    return a;
}

QVector<Answer> AskUserFormWidget::answers() const
{
    QVector<Answer> out;
    out.reserve(m_spec.questions.size());
    for (int i = 0; i < m_spec.questions.size(); ++i)
        out.append(answerAt(i));
    return out;
}

void AskUserFormWidget::trySubmit()
{
    if (!isPending())
        return;
    const QVector<Answer> current = answers();
    int firstMissing = -1;
    for (int i = 0; i < m_spec.questions.size(); ++i) {
        const Question &q = m_spec.questions.at(i);
        const Row &row = m_rows.at(i);
        const Answer &a = current.at(i);
        // A requested change must say what to change.
        const bool missingChange = q.type == QuestionType::Proposal
            && a.value.toString() == QLatin1String("change") && a.other.isEmpty();
        const bool missing = missingChange || (q.required && !AskUserForm::hasAnswer(q, a));
        row.requiredLabel->setText(missingChange ? tr("Describe the change") : tr("Required"));
        row.requiredLabel->setVisible(q.required || missingChange);
        QFont f = row.requiredLabel->font();
        f.setBold(missing);
        row.requiredLabel->setFont(f);
        if (missing && firstMissing < 0)
            firstMissing = i;
    }
    if (firstMissing >= 0) {
        if (QWidget *target = inputOf(firstMissing))
            target->setFocus();
        return;
    }
    emit submitted(current);
}

bool AskUserFormWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress || !isPending())
        return QFrame::eventFilter(watched, event);
    auto *ke = static_cast<QKeyEvent *>(event);
    if (ke->key() == Qt::Key_Escape) {
        emit escapePressed();
        return true;
    }
    if (ke->key() != Qt::Key_Return && ke->key() != Qt::Key_Enter)
        return QFrame::eventFilter(watched, event);
    if (ke->modifiers() & Qt::ShiftModifier) {
        // Shift+Enter: a real newline (QPlainTextEdit would insert U+2028).
        if (auto *edit = qobject_cast<QPlainTextEdit *>(watched)) {
            edit->insertPlainText(QStringLiteral("\n"));
            return true;
        }
        return QFrame::eventFilter(watched, event);
    }
    trySubmit();
    return true;
}

void AskUserFormWidget::setExpanded(bool expanded)
{
    m_toggle->setText(expanded ? QStringLiteral("▾") : QStringLiteral("▸"));
    m_body->setVisible(expanded);
    m_footer->setVisible(expanded && isPending());
}

QString AskUserFormWidget::statusText(const QString &detail) const
{
    if (!detail.isEmpty())
        return detail;
    switch (m_state) {
    case State::Pending: return {};
    case State::Answered: return tr("Answered");
    case State::AnsweredInChat: return tr("Answered in chat");
    case State::Dismissed: return tr("Dismissed");
    case State::ClarifyRequested: return tr("Clarification requested");
    case State::Superseded: return tr("Superseded by a newer form");
    case State::Cancelled: return tr("Cancelled");
    case State::Expired: return tr("Expired");
    case State::Invalid: return tr("Invalid form");
    }
    return {};
}

void AskUserFormWidget::setState(State state, const QString &detail)
{
    m_state = state;
    const bool pending = state == State::Pending;
    const QString text = statusText(detail);
    m_statusLabel->setText(text);
    m_statusLabel->setVisible(!text.isEmpty());
    m_body->setEnabled(pending);
    // Settled forms collapse to their status line; the toggle still shows them.
    m_toggle->setChecked(pending);
    setExpanded(pending);
}

void AskUserFormWidget::setChatFont(const QFont &font)
{
    // Content follows the chat font; chrome (buttons) keeps the system font.
    const QList<QWidget *> children = m_body->findChildren<QWidget *>();
    for (QWidget *w : children) {
        if (qobject_cast<QPushButton *>(w) || qobject_cast<QToolButton *>(w))
            continue;
        QFont f = font;
        f.setWeight(w->font().weight());
        w->setFont(f);
    }
    QFont title = font;
    title.setWeight(QFont::DemiBold);
    m_titleLabel->setFont(title);
    m_statusLabel->setFont(font);
    for (Row &row : m_rows) {
        if (row.textEdit)
            fitLines(row.textEdit, kTextareaLines);
    }
}

QWidget *AskUserFormWidget::inputOf(int index) const
{
    const Row &row = m_rows.at(index);
    // A requested change is answered in its text box.
    if (row.textEdit && !row.textEdit->isHidden())
        return row.textEdit;
    if (row.group && row.group->checkedButton())
        return row.group->checkedButton();
    if (!row.optionButtons.isEmpty())
        return row.optionButtons.first();
    if (row.combo)
        return row.combo;
    if (row.lineEdit)
        return row.lineEdit;
    return row.textEdit;
}

void AskUserFormWidget::focusFirstQuestion()
{
    if (m_rows.isEmpty())
        return;
    if (QWidget *target = inputOf(0))
        target->setFocus();
}
