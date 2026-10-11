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

#include <QFont>

#include "AcpProtocol.h"

AcpTurnFoldRow::AcpTurnFoldRow(qint64 workedMs, QWidget *parent)
    : QPushButton(parent)
    , m_workedMs(workedMs)
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

void AcpTurnFoldRow::setChatFont(const QFont &font)
{
    setFont(font);
}

void AcpTurnFoldRow::refreshText()
{
    const QString label = m_workedMs >= 0
        ? tr("Worked for %1").arg(AcpProtocol::formatToolDuration(m_workedMs))
        : tr("Worked");
    setText(label + (m_expanded ? QStringLiteral(" ⌄") : QStringLiteral(" ›")));
    setToolTip(m_expanded ? tr("Hide tool calls") : tr("Show tool calls"));
}
