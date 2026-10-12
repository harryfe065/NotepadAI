/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef TERMINALSEARCHBAR_H
#define TERMINALSEARCHBAR_H

#include "TerminalSearch.h"

#include <QFrame>

class QLabel;
class QLineEdit;
class QToolButton;

// Floating find bar in the terminal's top-right corner (VS Code style):
// query, case / regex / whole-word toggles, "n/m" count, previous / next and
// close. Enter / Shift+Enter step, Esc closes.
class TerminalSearchBar : public QFrame
{
    Q_OBJECT

public:
    explicit TerminalSearchBar(QWidget *parent = nullptr);

    TerminalSearchOptions options() const;
    void setQuery(const QString &query);
    void focusQuery();
    // `active` is 0-based; -1 with total 0 shows "No results".
    void setCount(int active, int total);
    void setInvalid(bool invalid);

signals:
    void optionsChanged();
    void nextRequested();
    void previousRequested();
    void closeRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    QToolButton *makeToggle(const QString &text, const QString &toolTip);
    QToolButton *makeButton(const QString &toolTip);
    void retint();

    QLineEdit *m_edit = nullptr;
    QToolButton *m_case = nullptr;
    QToolButton *m_regex = nullptr;
    QToolButton *m_word = nullptr;
    QLabel *m_count = nullptr;
    QToolButton *m_prev = nullptr;
    QToolButton *m_next = nullptr;
    QToolButton *m_close = nullptr;
};

#endif
