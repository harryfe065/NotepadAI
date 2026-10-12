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
#include <QDir>
#include <QDoubleValidator>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QWheelEvent>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

using AskUserForm::Answer;
using AskUserForm::Question;
using AskUserForm::QuestionType;

namespace {

constexpr int kTextareaLines = 4;
constexpr qreal kClarifyOpacity = 0.45; // controls of a flagged question: not sent
constexpr int kOtherButtonId = 1 << 20; // past any option index; never a Qt auto id

// Dropdown item data of the "Other…" entry; never a valid option value.
const QString &otherItemData()
{
    static const QString kData = QStringLiteral("\u0001other");
    return kData;
}

void fitLines(QPlainTextEdit *edit, int lines)
{
    const QFontMetrics fm(edit->font());
    const int frame = 2 * edit->frameWidth();
    const int margins = qRound(2 * edit->document()->documentMargin());
    edit->setFixedHeight(fm.lineSpacing() * lines + frame + margins + 8); // + stylesheet padding
}

void repolish(QWidget *w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(static_cast<float>(a.redF() + (b.redF() - a.redF()) * t),
                            static_cast<float>(a.greenF() + (b.greenF() - a.greenF()) * t),
                            static_cast<float>(a.blueF() + (b.blueF() - a.blueF()) * t));
}

QString css(const QColor &c)
{
    return QStringLiteral("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
}

// A 12x12 stroked SVG glyph in `color` for style-sheet `image:` rules. Style
// sheets cannot tint an image, so one file per glyph and colour is written to
// the cache dir once per process (a few hundred bytes) and reused.
QString glyphImage(const char *name, const char *pathData, const QColor &color)
{
    static QHash<QString, QString> cache;
    const QString key = QLatin1String(name) + color.name();
    if (const auto it = cache.constFind(key); it != cache.constEnd())
        return it.value();
    QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (dir.isEmpty() || !QDir().mkpath(dir))
        dir = QDir::tempPath();
    const QString path = dir + QStringLiteral("/ask-user-%1-%2.svg").arg(QLatin1String(name), color.name().mid(1));
    if (!QFileInfo::exists(path)) {
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QStringLiteral(
                "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"12\" height=\"12\" viewBox=\"0 0 12 12\">"
                "<path d=\"%1\" fill=\"none\" stroke=\"%2\" stroke-width=\"1.9\""
                " stroke-linecap=\"round\" stroke-linejoin=\"round\"/></svg>")
                           .arg(QLatin1String(pathData), color.name())
                           .toUtf8());
            file.commit();
        }
    }
    cache.insert(key, path);
    return path;
}

// Scrolling the transcript over a dropdown must scroll, not change the pick.
class NoWheelComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;

protected:
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};

// One choice as a rounded, clickable row: the indicator button and the
// description underneath. A click anywhere on the
// row toggles the button; the row restyles itself from the button's state.
class OptionRow : public QFrame
{
public:
    OptionRow(QAbstractButton *button, QWidget *parent)
        : QFrame(parent)
        , m_button(button)
    {
        setObjectName(QStringLiteral("auOption"));
        setCursor(Qt::PointingHandCursor);
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(10, 7, 10, 7);
        m_layout->setSpacing(2);
        m_top = new QHBoxLayout;
        m_top->setSpacing(8);
        m_top->addWidget(button);
        m_layout->addLayout(m_top);
        QObject::connect(button, &QAbstractButton::toggled, this, [this](bool on) {
            setProperty("checked", on);
            repolish(this);
        });
    }

    void addStretch() { m_top->addStretch(1); }
    void addTopWidget(QWidget *w, int stretch = 0) { m_top->addWidget(w, stretch); }
    void addBelow(QWidget *w) { m_layout->addWidget(w); }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_button->isEnabled()) {
            if (m_button->isCheckable() && qobject_cast<QRadioButton *>(m_button))
                m_button->setChecked(true);
            else
                m_button->toggle();
            m_button->setFocus(Qt::MouseFocusReason);
            event->accept();
            return;
        }
        QFrame::mousePressEvent(event);
    }

private:
    QAbstractButton *m_button;
    QVBoxLayout *m_layout = nullptr;
    QHBoxLayout *m_top = nullptr;
};

// The recommended answer as the user sees it ("Yes", "radio, checkbox", ...).
QString recommendedText(const Question &q)
{
    const QJsonValue &rec = q.recommended;
    auto labelOf = [&q](const QString &value) {
        for (const AskUserForm::Option &opt : q.options) {
            if (opt.value == value)
                return opt.label;
        }
        return value;
    };
    switch (q.type) {
    case QuestionType::Single:
    case QuestionType::Select:
        return rec.isString() ? labelOf(rec.toString()) : QString();
    case QuestionType::Multi: {
        QStringList labels;
        for (const QJsonValue &v : rec.toArray())
            labels.append(labelOf(v.toString()));
        return labels.join(QStringLiteral(", "));
    }
    case QuestionType::Confirm:
        if (!rec.isBool())
            return {};
        if (rec.toBool())
            return q.yesLabel.isEmpty() ? AskUserFormWidget::tr("Yes") : q.yesLabel;
        return q.noLabel.isEmpty() ? AskUserFormWidget::tr("No") : q.noLabel;
    case QuestionType::Proposal:
        return rec.toString() == QLatin1String("change") ? AskUserFormWidget::tr("Change")
                                                         : AskUserFormWidget::tr("Accept");
    case QuestionType::Text:
    case QuestionType::Textarea:
        return rec.toString().trimmed().isEmpty() ? QString() : QStringLiteral("\u201c%1\u201d").arg(rec.toString());
    case QuestionType::Number:
        return rec.isDouble() ? QString::number(rec.toDouble(), 'g', 15) : QString();
    }
    return {};
}

// Option labels are plain text; keep them literal inside the markdown line.
QString escapeMarkdown(QString text)
{
    for (const QChar c : {QLatin1Char('\\'), QLatin1Char('*'), QLatin1Char('_'), QLatin1Char('`'),
                          QLatin1Char('['), QLatin1Char(']'), QLatin1Char('<'), QLatin1Char('#')}) {
        text.replace(c, QString(QLatin1Char('\\')) + c);
    }
    return text;
}

// Marks the recommended choice next to its label; the footnote line says why.
QString withMark(const QString &label, bool recommended)
{
    return recommended ? label + QStringLiteral(" *") : label;
}

} // namespace

AskUserFormWidget::AskUserFormWidget(AskUserForm::Spec spec, const QVector<Answer> &initial,
                                     QWidget *parent)
    : QFrame(parent)
    , m_spec(std::move(spec))
{
    setObjectName(QStringLiteral("askUserForm"));
    setProperty("pending", true);
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
    m_building = false;
    applyTheme();
    updateProgress();
}

QLabel *AskUserFormWidget::mutedLabel(const QString &text, Qt::TextFormat format, QWidget *parent) const
{
    auto *label = new QLabel(parent);
    label->setObjectName(QStringLiteral("auMuted"));
    label->setTextFormat(format);
    label->setText(text);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    label->setOpenExternalLinks(true);
    return label;
}

void AskUserFormWidget::applyTheme()
{
    // Computed from the live palette (light and dark), refreshed on
    // PaletteChange: no single palette() role contrasts in both themes.
    const QPalette pal = palette();
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    const QColor accent = pal.color(QPalette::Highlight);
    const QColor onAccent = pal.color(QPalette::HighlightedText);
    const QColor muted = pal.color(QPalette::PlaceholderText);
    // setStyleSheet itself posts a PaletteChange: rebuild only on a real change.
    const QString themeKey = base.name() + text.name() + accent.name() + onAccent.name() + muted.name();
    if (themeKey == m_themeKey)
        return;
    m_themeKey = themeKey;
    const QColor border = mix(base, text, 0.16);
    const QColor borderStrong = mix(base, text, 0.42);
    const QColor hover = mix(base, text, 0.06);
    const QColor input = mix(base, text, 0.03);
    const QColor accentTint = mix(base, accent, 0.18);
    const QColor accentLine = mix(base, accent, 0.75);
    const QColor accentText = mix(accent, text, 0.35); // readable on base and tint
    const QColor accentHover = mix(accent, text, 0.12);

    QString sheet = QStringLiteral(
        "QFrame#askUserForm { background: %BASE%; border: 1px solid %BORDER%; border-radius: 8px; }"
        "QFrame#askUserForm[pending=\"true\"] { border-color: %ACCENTLINE%; }"
        "QLabel#auBadge { background: %ACCENT%; color: %ONACCENT%; border-radius: 9px;"
        " min-width: 18px; max-width: 18px; min-height: 18px; max-height: 18px; font-weight: 600; }"
        "QFrame#askUserForm[pending=\"false\"] QLabel#auBadge { background: %HOVER%; color: %MUTED%; }"
        "QLabel#auNumber { background: %ACCENTTINT%; color: %ACCENTTEXT%; border-radius: 4px;"
        " padding: 0px 6px; font-weight: 600; }"
        "QLabel#auMuted, QLabel#auStatus { color: %MUTED%; }"
        "QLabel#auRequired { color: %MUTED%; }"
        "QWidget#auQuestion[invalid=\"true\"] QLabel#auRequired { color: %ACCENTTEXT%; font-weight: 600; }"
        "QLabel#auRecommendation { color: %MUTED%; font-style: italic; }"
        "QFrame#auOption { background: transparent; border: 1px solid %BORDER%; border-radius: 6px; }"
        "QFrame#auOption:hover { background: %HOVER%; }"
        "QFrame#auOption[checked=\"true\"] { background: %ACCENTTINT%; border-color: %ACCENTLINE%; }"
        "QFrame#auOption QRadioButton, QFrame#auOption QCheckBox { background: transparent; spacing: 10px; }"
        "QRadioButton::indicator, QCheckBox::indicator { width: 12px; height: 12px; background: %INPUT%;"
        " border: 2px solid %BORDERSTRONG%; }"
        "QRadioButton::indicator { border-radius: 8px; }"
        "QCheckBox::indicator { border-radius: 3px; }"
        "QRadioButton::indicator:hover, QCheckBox::indicator:hover { border-color: %ACCENT%; }"
        "QRadioButton::indicator:checked { background: %ONACCENT%; border: 5px solid %ACCENT%;"
        " width: 6px; height: 6px; }"
        "QCheckBox::indicator:checked { background: %ACCENT%; border-color: %ACCENT%;"
        " image: url(\"%CHECKMARK%\"); }"
        "QPushButton#auSeg { background: transparent; color: %TEXT%; border: 1px solid %BORDER%;"
        " border-radius: 6px; padding: 5px 16px; }"
        "QPushButton#auSeg:hover { background: %HOVER%; }"
        "QPushButton#auSeg:checked { background: %ACCENT%; color: %ONACCENT%; border-color: %ACCENT%; }"
        "QPushButton#auPrimary { background: %ACCENT%; color: %ONACCENT%; border: none; border-radius: 6px;"
        " padding: 6px 18px; font-weight: 600; }"
        "QPushButton#auPrimary:hover { background: %ACCENTHOVER%; }"
        "QPushButton#auGhost { background: transparent; color: %MUTED%; border: none; border-radius: 6px;"
        " padding: 6px 12px; }"
        "QPushButton#auGhost:hover { background: %HOVER%; color: %TEXT%; }"
        "QToolButton#auLink { background: transparent; color: %MUTED%; border: none; border-radius: 4px;"
        " padding: 1px 6px; }"
        "QToolButton#auLink:hover, QToolButton#auLink:checked { background: %HOVER%; color: %ACCENTTEXT%; }"
        "QToolButton#auToggle { background: transparent; color: %MUTED%; border: none; padding: 0px 4px; }"
        "QToolButton#auToggle:hover { color: %TEXT%; }"
        "QFrame#askUserForm QLineEdit, QFrame#askUserForm QPlainTextEdit { background: %INPUT%;"
        " color: %TEXT%; border: 1px solid %BORDER%; border-radius: 6px; padding: 4px 6px; }"
        "QFrame#askUserForm QLineEdit:focus, QFrame#askUserForm QPlainTextEdit:focus { border-color: %ACCENT%; }"
        "QFrame#askUserForm QComboBox { background: %INPUT%; color: %TEXT%; border: 1px solid %BORDER%;"
        " border-radius: 6px; padding: 5px 28px 5px 8px; }"
        "QFrame#askUserForm QComboBox:hover { border-color: %BORDERSTRONG%; }"
        "QFrame#askUserForm QComboBox:focus, QFrame#askUserForm QComboBox:on { border-color: %ACCENT%; }"
        "QFrame#askUserForm QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right;"
        " width: 24px; border: none; background: transparent; }"
        "QFrame#askUserForm QComboBox::down-arrow { image: url(\"%CHEVRON%\"); width: 12px; height: 12px; }"
        "QFrame#askUserForm QComboBox QAbstractItemView { background: %BASE%; color: %TEXT%;"
        " border: 1px solid %BORDER%; padding: 4px; outline: 0; }"
        "QFrame#askUserForm QComboBox QAbstractItemView::item { min-height: 26px; padding: 0px 8px;"
        " border-radius: 4px; }"
        "QFrame#askUserForm QComboBox QAbstractItemView::item:hover,"
        " QFrame#askUserForm QComboBox QAbstractItemView::item:selected { background: %ACCENTTINT%;"
        " color: %TEXT%; }");
    const std::pair<const char *, QColor> vars[] = {
        {"%BASE%", base},           {"%BORDERSTRONG%", borderStrong}, {"%BORDER%", border},
        {"%HOVER%", hover},
        {"%INPUT%", input},         {"%ACCENTTINT%", accentTint}, {"%ACCENTLINE%", accentLine},
        {"%ACCENTTEXT%", accentText}, {"%ACCENTHOVER%", accentHover}, {"%ACCENT%", accent},
        {"%ONACCENT%", onAccent},   {"%MUTED%", muted},           {"%TEXT%", text},
    };
    for (const auto &[key, color] : vars)
        sheet.replace(QLatin1String(key), css(color));
    sheet.replace(QLatin1String("%CHECKMARK%"), glyphImage("check", "M2.5 6.3 L5 8.8 L9.6 3.4", onAccent));
    sheet.replace(QLatin1String("%CHEVRON%"), glyphImage("chevron", "M3 4.6 L6 7.6 L9 4.6", muted));
    setStyleSheet(sheet);
}

void AskUserFormWidget::changeEvent(QEvent *event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && !m_building)
        applyTheme();
}

void AskUserFormWidget::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 12);
    root->setSpacing(10);

    // Header: badge, title, status / progress, collapse chevron.
    auto *header = new QHBoxLayout;
    header->setSpacing(8);
    m_badge = new QLabel(QStringLiteral("?"), this);
    m_badge->setObjectName(QStringLiteral("auBadge"));
    m_badge->setAlignment(Qt::AlignCenter);
    header->addWidget(m_badge);

    const qsizetype count = m_spec.questions.size();
    QString title = m_spec.title;
    if (title.isEmpty())
        title = count <= 1 ? tr("Question for you") : tr("%1 questions for you").arg(count);
    m_titleLabel = new QLabel(title, this);
    m_titleLabel->setWordWrap(true);
    QFont bold = m_titleLabel->font();
    bold.setWeight(QFont::DemiBold);
    m_titleLabel->setFont(bold);
    header->addWidget(m_titleLabel, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("auStatus"));
    m_statusLabel->hide();
    header->addWidget(m_statusLabel);

    m_toggle = new QToolButton(this);
    m_toggle->setObjectName(QStringLiteral("auToggle"));
    m_toggle->setCheckable(true);
    m_toggle->setChecked(true);
    m_toggle->setText(QStringLiteral("▾"));
    m_toggle->setToolTip(tr("Show or hide the questions"));
    m_toggle->setCursor(Qt::PointingHandCursor);
    m_toggle->hide(); // only settled forms collapse
    connect(m_toggle, &QToolButton::toggled, this, &AskUserFormWidget::setExpanded);
    header->addWidget(m_toggle);
    root->addLayout(header);

    m_body = new QWidget(this);
    auto *bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(16);
    m_rows.resize(m_spec.questions.size());
    for (int i = 0; i < m_spec.questions.size(); ++i)
        bodyLayout->addWidget(buildQuestion(i));
    root->addWidget(m_body);

    m_footer = new QWidget(this);
    auto *footer = new QHBoxLayout(m_footer);
    footer->setContentsMargins(0, 2, 0, 0);
    footer->setSpacing(6);
    m_progressLabel = mutedLabel(QString(), Qt::PlainText, m_footer);
    m_progressLabel->setWordWrap(false);
    footer->addWidget(m_progressLabel, 1);
    m_dismissButton = new QPushButton(tr("Dismiss"), m_footer);
    m_dismissButton->setObjectName(QStringLiteral("auGhost"));
    m_dismissButton->setAutoDefault(false);
    m_dismissButton->setCursor(Qt::PointingHandCursor);
    m_dismissButton->setToolTip(tr("Tell the agent you will not answer this form"));
    connect(m_dismissButton, &QPushButton::clicked, this, [this]() {
        if (isPending())
            emit dismissed();
    });
    footer->addWidget(m_dismissButton);
    m_submitButton = new QPushButton(tr("Submit  ↵"), m_footer);
    m_submitButton->setObjectName(QStringLiteral("auPrimary"));
    m_submitButton->setAutoDefault(false);
    m_submitButton->setCursor(Qt::PointingHandCursor);
    m_submitButton->setToolTip(tr("Enter submits · Shift+Enter adds a new line"));
    connect(m_submitButton, &QPushButton::clicked, this, &AskUserFormWidget::trySubmit);
    footer->addWidget(m_submitButton);
    root->addWidget(m_footer);
}

QWidget *AskUserFormWidget::buildQuestion(int index)
{
    const Question &q = m_spec.questions.at(index);
    Row &row = m_rows[index];
    row.container = new QWidget(m_body);
    row.container->setObjectName(QStringLiteral("auQuestion"));
    auto *layout = new QVBoxLayout(row.container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // Title row: number chip, title, Required, Note, Clarify.
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    const bool numbered = m_spec.source == AskUserForm::Source::AskUser || m_spec.questions.size() > 1;
    if (numbered) {
        const QString label = m_spec.source == AskUserForm::Source::AskUser
            ? q.id : QStringLiteral("Q%1").arg(index + 1);
        auto *number = new QLabel(label, row.container);
        number->setObjectName(QStringLiteral("auNumber"));
        titleRow->addWidget(number, 0, Qt::AlignVCenter);
    }
    auto *title = new QLabel(q.title, row.container);
    title->setWordWrap(true);
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont bold = title->font();
    bold.setWeight(QFont::DemiBold);
    title->setFont(bold);
    titleRow->addWidget(title, 1);

    row.requiredLabel = new QLabel(tr("Required"), row.container);
    row.requiredLabel->setObjectName(QStringLiteral("auRequired"));
    row.requiredLabel->setVisible(q.required);
    titleRow->addWidget(row.requiredLabel, 0, Qt::AlignVCenter);

    row.noteButton = new QToolButton(row.container);
    row.noteButton->setObjectName(QStringLiteral("auLink"));
    row.noteButton->setCheckable(true);
    row.noteButton->setText(tr("+ Note"));
    row.noteButton->setToolTip(tr("Add a note to this answer"));
    row.noteButton->setCursor(Qt::PointingHandCursor);
    // Elicitation without a free-text field cannot carry a note.
    const bool canNote = m_spec.source == AskUserForm::Source::AskUser
        || !q.noteFieldKey.isEmpty() || !q.otherFieldKey.isEmpty();
    row.noteButton->setVisible(canNote);
    titleRow->addWidget(row.noteButton, 0, Qt::AlignVCenter);

    // Elicitation carries the request in a free-text companion field.
    const bool canClarify = m_spec.source == AskUserForm::Source::AskUser
        || !q.otherFieldKey.isEmpty() || !q.noteFieldKey.isEmpty();
    if (canClarify) {
        row.clarifyButton = new QToolButton(row.container);
        row.clarifyButton->setObjectName(QStringLiteral("auLink"));
        row.clarifyButton->setCheckable(true);
        row.clarifyButton->setText(tr("Clarify"));
        row.clarifyButton->setToolTip(tr("Mark as unclear: the agent explains it after you submit"));
        row.clarifyButton->setCursor(Qt::PointingHandCursor);
        connect(row.clarifyButton, &QToolButton::toggled, this, [this, index](bool on) {
            setClarify(index, on);
        });
        titleRow->addWidget(row.clarifyButton, 0, Qt::AlignVCenter);
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

    const QString recommended = recommendedText(q);
    if (!recommended.isEmpty() || !q.recommendation.trimmed().isEmpty()) {
        // Markdown: the agent's reasoning often carries **bold** / `code`.
        QString line = QStringLiteral("\\* ") + tr("Recommended");
        if (!recommended.isEmpty())
            line += QStringLiteral(": ") + escapeMarkdown(recommended);
        if (!q.recommendation.trimmed().isEmpty())
            line += QStringLiteral(" \u2014 ") + q.recommendation.trimmed();
        auto *rec = new QLabel(row.container);
        rec->setObjectName(QStringLiteral("auRecommendation"));
        rec->setTextFormat(Qt::MarkdownText);
        rec->setText(line);
        rec->setWordWrap(true);
        rec->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont italic = rec->font();
        italic.setItalic(true);
        rec->setFont(italic);
        layout->addWidget(rec);
    }

    row.answerBox = new QWidget(row.container);
    auto *answerLayout = new QVBoxLayout(row.answerBox);
    answerLayout->setContentsMargins(0, 0, 0, 0);
    answerLayout->setSpacing(layout->spacing());
    layout->addWidget(row.answerBox);

    switch (q.type) {
    case QuestionType::Single:
    case QuestionType::Select:
        if (q.isDropdown())
            buildDropdown(index, row, answerLayout);
        else
            buildChoice(index, row, answerLayout);
        break;
    case QuestionType::Multi:
        buildChoice(index, row, answerLayout);
        break;
    case QuestionType::Confirm:
    case QuestionType::Proposal:
        buildSegmented(index, row, answerLayout);
        break;
    case QuestionType::Text:
    case QuestionType::Number: {
        row.lineEdit = new QLineEdit(row.container);
        row.lineEdit->setPlaceholderText(q.placeholder.isEmpty() ? tr("Type your answer") : q.placeholder);
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
        answerLayout->addWidget(row.lineEdit);
        watch(row.lineEdit);
        connect(row.lineEdit, &QLineEdit::textChanged, this, &AskUserFormWidget::updateProgress);
        break;
    }
    case QuestionType::Textarea:
        row.textEdit = new QPlainTextEdit(row.container);
        row.textEdit->setPlaceholderText(q.placeholder.isEmpty() ? tr("Type your answer") : q.placeholder);
        row.textEdit->setTabChangesFocus(true);
        fitLines(row.textEdit, kTextareaLines);
        answerLayout->addWidget(row.textEdit);
        watch(row.textEdit);
        connect(row.textEdit, &QPlainTextEdit::textChanged, this, &AskUserFormWidget::updateProgress);
        break;
    }

    row.noteEdit = new QLineEdit(row.container);
    row.noteEdit->setPlaceholderText(tr("Note for the agent"));
    row.noteEdit->hide();
    layout->addWidget(row.noteEdit);
    watch(row.noteEdit);
    QLineEdit *noteEdit = row.noteEdit;
    QToolButton *noteButton = row.noteButton;
    connect(row.noteButton, &QToolButton::toggled, this, [noteEdit, noteButton](bool on) {
        noteEdit->setVisible(on);
        noteButton->setText(on ? AskUserFormWidget::tr("− Note") : AskUserFormWidget::tr("+ Note"));
        if (on)
            noteEdit->setFocus();
    });
    return row.container;
}

void AskUserFormWidget::buildChoice(int index, Row &row, QVBoxLayout *layout)
{
    const Question &q = m_spec.questions.at(index);
    const bool multi = q.type == QuestionType::Multi;
    if (!multi) {
        row.group = new QButtonGroup(row.container);
        connect(row.group, &QButtonGroup::idToggled, this, &AskUserFormWidget::updateProgress);
    }
    for (int i = 0; i < q.options.size(); ++i) {
        const AskUserForm::Option &opt = q.options.at(i);
        const bool recommended = multi ? q.recommended.toArray().contains(opt.value)
                                       : q.recommended.toString() == opt.value;
        QAbstractButton *button = multi
            ? static_cast<QAbstractButton *>(new QCheckBox(withMark(opt.label, recommended)))
            : static_cast<QAbstractButton *>(new QRadioButton(withMark(opt.label, recommended)));
        auto *option = new OptionRow(button, row.container);
        option->addStretch();
        if (!opt.description.trimmed().isEmpty()) {
            auto *desc = new QLabel(option);
            desc->setObjectName(QStringLiteral("auMuted"));
            desc->setTextFormat(Qt::MarkdownText);
            desc->setText(opt.description);
            desc->setWordWrap(true);
            // Not selectable: a click on the description selects the option.
            desc->setContentsMargins(button->style()->pixelMetric(QStyle::PM_IndicatorWidth) + 8, 0, 0, 0);
            option->addBelow(desc);
        }
        if (row.group)
            row.group->addButton(button, i);
        else
            connect(button, &QAbstractButton::toggled, this, &AskUserFormWidget::updateProgress);
        row.optionButtons.append(button);
        layout->addWidget(option);
        watch(button);
    }
    if (q.allowOther)
        buildOther(row, layout, multi);
}

void AskUserFormWidget::buildDropdown(int index, Row &row, QVBoxLayout *layout)
{
    const Question &q = m_spec.questions.at(index);
    row.combo = new NoWheelComboBox(row.container);
    // Item rules in the style sheet apply only with a styled delegate.
    row.combo->setItemDelegate(new QStyledItemDelegate(row.combo));
    row.combo->addItem(tr("Choose…"), QString()); // index 0 = no answer
    for (const AskUserForm::Option &opt : q.options) {
        const bool recommended = q.recommended.toString() == opt.value;
        row.combo->addItem(withMark(opt.label, recommended), opt.value);
        if (!opt.description.isEmpty())
            row.combo->setItemData(row.combo->count() - 1, opt.description, Qt::ToolTipRole);
    }
    if (q.allowOther)
        row.combo->addItem(tr("Other…"), otherItemData());
    layout->addWidget(row.combo);
    watch(row.combo);
    connect(row.combo, &QComboBox::currentIndexChanged, this, &AskUserFormWidget::updateProgress);
    if (q.allowOther) {
        row.otherEdit = new QLineEdit(row.container);
        row.otherEdit->setPlaceholderText(tr("Type your own answer"));
        row.otherEdit->hide();
        layout->addWidget(row.otherEdit);
        watch(row.otherEdit);
        connect(row.otherEdit, &QLineEdit::textChanged, this, &AskUserFormWidget::updateProgress);
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

void AskUserFormWidget::buildSegmented(int index, Row &row, QVBoxLayout *layout)
{
    const Question &q = m_spec.questions.at(index);
    const bool proposal = q.type == QuestionType::Proposal;
    const QString yes = proposal ? tr("Accept") : (q.yesLabel.isEmpty() ? tr("Yes") : q.yesLabel);
    const QString no = proposal ? tr("Change…") : (q.noLabel.isEmpty() ? tr("No") : q.noLabel);
    row.group = new QButtonGroup(row.container);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    const bool hasRecommendation = q.recommended.isBool() || q.recommended.isString();
    const bool recommendYes = proposal ? q.recommended.toString() != QLatin1String("change")
                                       : q.recommended.toBool();
    for (int i = 0; i < 2; ++i) {
        const bool marked = hasRecommendation && (i == 0) == recommendYes;
        auto *button = new QPushButton(withMark(i == 0 ? yes : no, marked), row.container);
        button->setObjectName(QStringLiteral("auSeg"));
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        if (marked)
            button->setToolTip(tr("Recommended"));
        row.group->addButton(button, i);
        row.optionButtons.append(button);
        buttons->addWidget(button);
        watch(button);
    }
    buttons->addStretch(1);
    layout->addLayout(buttons);
    connect(row.group, &QButtonGroup::idToggled, this, &AskUserFormWidget::updateProgress);
    if (proposal) {
        row.textEdit = new QPlainTextEdit(row.container);
        row.textEdit->setPlaceholderText(tr("What should change?"));
        row.textEdit->setTabChangesFocus(true);
        fitLines(row.textEdit, kTextareaLines);
        row.textEdit->hide();
        layout->addWidget(row.textEdit);
        watch(row.textEdit);
        connect(row.textEdit, &QPlainTextEdit::textChanged, this, &AskUserFormWidget::updateProgress);
        connect(row.group, &QButtonGroup::idToggled, this, [this, index](int, bool) {
            updateProposalEditor(index);
        });
    }
}

void AskUserFormWidget::buildOther(Row &row, QVBoxLayout *layout, bool checkbox)
{
    row.otherButton = checkbox ? static_cast<QAbstractButton *>(new QCheckBox(tr("Other")))
                               : static_cast<QAbstractButton *>(new QRadioButton(tr("Other")));
    auto *option = new OptionRow(row.otherButton, row.container);
    row.otherEdit = new QLineEdit(option);
    row.otherEdit->setPlaceholderText(tr("Type your own answer"));
    option->addTopWidget(row.otherEdit, 1);
    if (row.group)
        row.group->addButton(row.otherButton, kOtherButtonId);
    else
        connect(row.otherButton, &QAbstractButton::toggled, this, &AskUserFormWidget::updateProgress);
    layout->addWidget(option);
    watch(row.otherButton);
    watch(row.otherEdit);
    QAbstractButton *button = row.otherButton;
    // Typing an answer selects Other.
    connect(row.otherEdit, &QLineEdit::textEdited, this, [button](const QString &text) {
        if (!text.isEmpty())
            button->setChecked(true);
    });
    connect(row.otherEdit, &QLineEdit::textChanged, this, &AskUserFormWidget::updateProgress);
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
    if (change && isPending() && !m_building)
        row.textEdit->setFocus();
}

void AskUserFormWidget::setClarify(int index, bool on)
{
    Row &row = m_rows[index];
    // The effect renders offscreen: only flagged rows carry an enabled one.
    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(row.answerBox->graphicsEffect());
    if (on && !effect) {
        effect = new QGraphicsOpacityEffect(row.answerBox);
        effect->setOpacity(kClarifyOpacity);
        row.answerBox->setGraphicsEffect(effect);
    }
    if (effect)
        effect->setEnabled(on);
    row.clarifyButton->setToolTip(on ? tr("Marked as unclear: click to answer it instead")
                                     : tr("Mark as unclear: the agent explains it after you submit"));
    if (on && row.container->property("invalid").toBool()) {
        row.container->setProperty("invalid", false);
        repolish(row.container);
        repolish(row.requiredLabel);
    }
    updateProgress();
}

void AskUserFormWidget::updateProgress()
{
    if (m_building || !m_progressLabel)
        return;
    int answered = 0;
    int clarify = 0;
    for (int i = 0; i < m_spec.questions.size(); ++i) {
        const Answer a = answerAt(i);
        if (a.clarify)
            ++clarify;
        else if (AskUserForm::hasAnswer(m_spec.questions.at(i), a))
            ++answered;
    }
    const qsizetype total = m_spec.questions.size();
    QStringList parts;
    if (total > 1)
        parts.append(tr("%1 of %2 answered").arg(answered).arg(total));
    if (clarify > 0)
        parts.append(tr("%1 to clarify").arg(clarify));
    parts.append(tr("Enter to submit"));
    m_progressLabel->setText(parts.join(QStringLiteral(" · ")));
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
    if (row.clarifyButton)
        row.clarifyButton->setChecked(a.clarify);
}

Answer AskUserFormWidget::answerAt(int index) const
{
    const Question &q = m_spec.questions.at(index);
    const Row &row = m_rows.at(index);
    Answer a;
    a.questionId = q.id;
    a.note = row.noteEdit ? row.noteEdit->text().trimmed() : QString();
    a.clarify = row.clarifyButton && row.clarifyButton->isChecked();
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
        // A requested change must say what to change. A question flagged
        // for clarification needs no answer.
        const bool missingChange = !a.clarify && q.type == QuestionType::Proposal
            && a.value.toString() == QLatin1String("change") && a.other.isEmpty();
        const bool missing = missingChange || (!a.clarify && q.required && !AskUserForm::hasAnswer(q, a));
        row.requiredLabel->setText(missingChange ? tr("Describe the change") : tr("Required"));
        row.requiredLabel->setVisible(q.required || missingChange);
        row.container->setProperty("invalid", missing);
        repolish(row.container);
        repolish(row.requiredLabel);
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
    switch (state) {
    case State::Pending: m_badge->setText(QStringLiteral("?")); break;
    case State::Answered:
    case State::AnsweredInChat: m_badge->setText(QStringLiteral("✓")); break;
    case State::ClarifyRequested: m_badge->setText(QStringLiteral("↻")); break;
    default: m_badge->setText(QStringLiteral("–")); break;
    }
    setProperty("pending", pending);
    repolish(this);
    m_body->setEnabled(pending);
    // Settled forms collapse to their status line; the chevron shows them.
    m_toggle->setVisible(!pending);
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
        f.setItalic(w->font().italic());
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
