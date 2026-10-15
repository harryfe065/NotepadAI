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

#include "AcpDiffStat.h"

class QFont;
class QTimer;

// "Worked for 16s ›" — the one-line stand-in for a finished turn's tool calls
// and intermediate messages. Pure chrome: the owner decides what expanding
// instantiates, so a collapsed turn costs a single widget.
//
// While its turn is still running the row is "live": a non-interactive
// "Worked for Xs" whose seconds tick once per second, no chevron. finishLive()
// freezes the total and turns it into the regular collapsible row. A finished
// row can go live again (a background wake-up continues its turn). A live row
// can also be interactive: the turn has folded, but background work it started
// (tasks, Monitor watches) still runs, so the seconds keep ticking while the
// row expands like a finished one.
class AcpTurnFoldRow : public QPushButton
{
    Q_OBJECT

public:
    // workedMs < 0 = unknown duration ("Worked").
    explicit AcpTurnFoldRow(qint64 workedMs, int toolCalls, QWidget *parent = nullptr);

    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded);

    // A turn with nothing folded under it is a plain label: no chevron, no click.
    void setExpandable(bool expandable);

    // Shown after the duration ("Worked for 16s · 5 tool calls"); 0 hides it.
    void setToolCallCount(int count);

    // Lines the turn's edits changed, shown last ("· +6 -10", the counts in the
    // diff colours); empty hides it.
    void setDiffStat(const AcpDiffStat::Lines &stat);

    // startMs: epoch ms the turn began (<= 0 = now). Non-interactive.
    void startLive(qint64 startMs);
    // Live only: keep ticking but expand on click (see the class comment).
    void setLiveInteractive(bool interactive);
    // endMs: epoch ms the turn ended (<= 0 = now).
    void finishLive(qint64 endMs = 0);
    bool isLive() const { return m_live; }
    bool isLiveInteractive() const { return m_live && m_liveInteractive; }

    // Styled widget: the transcript host's font does not reach it, so the
    // owner pushes the chat (Default Font) typeface down explicitly.
    void setChatFont(const QFont &font);

signals:
    // Emitted only for user-driven toggles, not for setExpanded().
    void expandedChanged(bool expanded);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void refreshText();
    void refreshInteraction();
    void onLiveTick();
    QString label(qint64 workedMs) const;
    bool clickable() const { return m_expandable && (!m_live || m_liveInteractive); }

    qint64 m_workedMs = -1;
    qint64 m_liveStartMs = 0;
    QTimer *m_liveTimer = nullptr;
    int m_toolCalls = 0;
    AcpDiffStat::Lines m_diffStat;
    bool m_expanded = false;
    bool m_expandable = true;
    bool m_live = false;
    bool m_liveInteractive = false;
};

#endif // ACP_TURN_FOLD_ROW_H
