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

#ifndef ACP_TURN_FOLD_ROW_H
#define ACP_TURN_FOLD_ROW_H

#include <QPushButton>

class QFont;

// "Worked for 16s ›" — the one-line stand-in for a finished turn's tool calls
// and intermediate messages. Pure chrome: the owner decides what expanding
// instantiates, so a collapsed turn costs a single widget.
class AcpTurnFoldRow : public QPushButton
{
    Q_OBJECT

public:
    // workedMs < 0 = unknown duration ("Worked").
    explicit AcpTurnFoldRow(qint64 workedMs, QWidget *parent = nullptr);

    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded);

    // Styled widget: the transcript host's font does not reach it, so the
    // owner pushes the chat (Default Font) typeface down explicitly.
    void setChatFont(const QFont &font);

signals:
    // Emitted only for user-driven toggles, not for setExpanded().
    void expandedChanged(bool expanded);

private:
    void refreshText();

    qint64 m_workedMs = -1;
    bool m_expanded = false;
};

#endif // ACP_TURN_FOLD_ROW_H
