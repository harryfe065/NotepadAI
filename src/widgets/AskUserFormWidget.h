/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef ASK_USER_FORM_WIDGET_H
#define ASK_USER_FORM_WIDGET_H

#include "AskUserForm.h"

#include <QFrame>
#include <QVector>

#include <cstdint>

class QAbstractButton;
class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;

// An interactive question form in the chat transcript (ask_user tool call or
// ACP form elicitation). Pending until the user submits, dismisses or asks
// for clarification; every other state is read-only and collapsed to its
// status line, expandable to show what was chosen.
class AskUserFormWidget : public QFrame
{
    Q_OBJECT

public:
    enum class State : std::uint8_t {
        Pending,
        Answered,
        AnsweredInChat,
        Dismissed,
        ClarifyRequested,
        Superseded,
        Cancelled,
        Expired,
        Invalid,
    };

    AskUserFormWidget(AskUserForm::Spec spec, const QVector<AskUserForm::Answer> &initial,
                      QWidget *parent = nullptr);

    const AskUserForm::Spec &spec() const { return m_spec; }
    State state() const { return m_state; }
    bool isPending() const { return m_state == State::Pending; }
    // `detail` overrides the default status text (e.g. the cancel reason).
    void setState(State state, const QString &detail = {});
    QVector<AskUserForm::Answer> answers() const;

    void setChatFont(const QFont &font);
    void focusFirstQuestion();

signals:
    void submitted(const QVector<AskUserForm::Answer> &answers);
    void dismissed();
    void clarifyRequested(int questionIndex);
    // Esc inside the form: give focus back to the composer.
    void escapePressed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    struct Row {
        QWidget *container = nullptr;
        QLabel *requiredLabel = nullptr;
        QButtonGroup *group = nullptr;          // single / confirm / proposal
        QVector<QAbstractButton *> optionButtons; // index == option index (confirm: yes, no)
        QComboBox *combo = nullptr;              // dropdown
        QAbstractButton *otherButton = nullptr;  // radio / checkbox for Other
        QLineEdit *otherEdit = nullptr;
        QLineEdit *lineEdit = nullptr;           // text / number
        QPlainTextEdit *textEdit = nullptr;      // textarea / proposal change
        QLineEdit *noteEdit = nullptr;
        QToolButton *noteButton = nullptr;
    };

    void buildUi();
    QWidget *buildQuestion(int index);
    void buildChoice(int index, Row &row, QVBoxLayout *layout);
    void buildDropdown(int index, Row &row, QVBoxLayout *layout);
    void buildSegmented(int index, Row &row, QVBoxLayout *layout);
    void buildOther(Row &row, QVBoxLayout *layout, bool checkbox);
    void applyAnswer(int index, const AskUserForm::Answer &answer);
    AskUserForm::Answer answerAt(int index) const;
    // The control that answers question `index` (focus target).
    QWidget *inputOf(int index) const;
    void trySubmit();
    void watch(QWidget *w);
    void updateProposalEditor(int index);
    void updateProgress();
    void setExpanded(bool expanded);
    void applyTheme();
    QString statusText(const QString &detail) const;
    QLabel *mutedLabel(const QString &text, Qt::TextFormat format, QWidget *parent) const;

    AskUserForm::Spec m_spec;
    QVector<Row> m_rows;
    State m_state = State::Pending;
    bool m_building = true;
    QString m_themeKey; // palette colours the current stylesheet was built from

    QLabel *m_badge = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QToolButton *m_toggle = nullptr;
    QWidget *m_body = nullptr;
    QWidget *m_footer = nullptr;
    QLabel *m_progressLabel = nullptr;
    QPushButton *m_submitButton = nullptr;
    QPushButton *m_dismissButton = nullptr;
};

#endif // ASK_USER_FORM_WIDGET_H
