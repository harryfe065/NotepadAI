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

#include "AcpTurnFoldRow.h"

#include <QDateTime>
#include <QFont>
#include <QTimer>

#include "AcpProtocol.h"

AcpTurnFoldRow::AcpTurnFoldRow(qint64 workedMs, int toolCalls, QWidget *parent)
    : QPushButton(parent)
    , m_workedMs(workedMs)
    , m_toolCalls(toolCalls)
{
    setObjectName(QStringLiteral("acpTurnFoldRow"));
    setFlat(true);
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::PointingHandCursor);
    // Muted whisper, no chrome: palette(placeholder-text) per ui-dna; the row
    // brightens on hover so it reads as clickable.
    setStyleSheet(QStringLiteral(
        "QPushButton#acpTurnFoldRow { border: none; background: transparent; text-align: left; "
        "padding: 4px 2px; color: palette(placeholder-text); }"
        "QPushButton#acpTurnFoldRow:hover { color: palette(text); }"));
    refreshText();
    connect(this, &QPushButton::clicked, this, [this]() {
        if (m_live || !m_expandable)
            return;
        m_expanded = !m_expanded;
        refreshText();
        emit expandedChanged(m_expanded);
    });
}

void AcpTurnFoldRow::setExpanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    m_expanded = expanded;
    refreshText();
}

void AcpTurnFoldRow::startLive(qint64 startMs)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_liveStartMs = (startMs > 0 && startMs <= now) ? startMs : now;
    m_live = true;
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::NoFocus);
    if (!m_liveTimer) {
        // One-shot, re-armed to the next whole-second boundary each tick: the
        // label never skips or repeats a second, and it costs one wakeup/s.
        m_liveTimer = new QTimer(this);
        m_liveTimer->setSingleShot(true);
        m_liveTimer->setTimerType(Qt::PreciseTimer);
        connect(m_liveTimer, &QTimer::timeout, this, &AcpTurnFoldRow::onLiveTick);
    }
    onLiveTick();
}

void AcpTurnFoldRow::finishLive(qint64 endMs)
{
    if (!m_live)
        return;
    m_live = false;
    if (m_liveTimer)
        m_liveTimer->stop();
    m_workedMs = qMax<qint64>(0, (endMs > 0 ? endMs : QDateTime::currentMSecsSinceEpoch()) - m_liveStartMs);
    setCursor(m_expandable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setFocusPolicy(m_expandable ? Qt::TabFocus : Qt::NoFocus);
    refreshText();
}

void AcpTurnFoldRow::setExpandable(bool expandable)
{
    if (m_expandable == expandable)
        return;
    m_expandable = expandable;
    if (!m_live) {
        setCursor(expandable ? Qt::PointingHandCursor : Qt::ArrowCursor);
        setFocusPolicy(expandable ? Qt::TabFocus : Qt::NoFocus);
        refreshText();
    }
}

void AcpTurnFoldRow::onLiveTick()
{
    if (!m_live)
        return;
    const qint64 elapsed = qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_liveStartMs);
    setText(label(elapsed));
    setToolTip(tr("Working…"));
    m_liveTimer->start(static_cast<int>(1000 - elapsed % 1000) + 1);
}

void AcpTurnFoldRow::setChatFont(const QFont &font)
{
    setFont(font);
}

void AcpTurnFoldRow::setToolCallCount(int count)
{
    if (m_toolCalls == count)
        return;
    m_toolCalls = count;
    if (m_live)
        setText(label(qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_liveStartMs)));
    else
        refreshText();
}

QString AcpTurnFoldRow::label(qint64 workedMs) const
{
    QString text = workedMs >= 0
        ? tr("Worked for %1").arg(AcpProtocol::formatToolDuration(workedMs))
        : tr("Worked");
    if (m_toolCalls > 0)
        text += QStringLiteral(" · ")
                + (m_toolCalls == 1 ? tr("1 tool call") : tr("%1 tool calls").arg(m_toolCalls));
    return text;
}

void AcpTurnFoldRow::refreshText()
{
    if (!m_expandable) {
        setText(label(m_workedMs));
        setToolTip(QString());
        return;
    }
    setText(label(m_workedMs) + (m_expanded ? QStringLiteral(" ⌄") : QStringLiteral(" ›")));
    setToolTip(m_expanded ? tr("Hide tool calls") : tr("Show tool calls"));
}
