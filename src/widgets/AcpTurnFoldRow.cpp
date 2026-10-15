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

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QStyleOptionButton>
#include <QStylePainter>
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
        if (!clickable())
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
    refreshInteraction();
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
    refreshInteraction();
    refreshText();
}

void AcpTurnFoldRow::setExpandable(bool expandable)
{
    if (m_expandable == expandable)
        return;
    m_expandable = expandable;
    refreshInteraction();
    refreshText();
}

void AcpTurnFoldRow::refreshInteraction()
{
    const bool on = clickable();
    setCursor(on ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setFocusPolicy(on ? Qt::TabFocus : Qt::NoFocus);
}

void AcpTurnFoldRow::onLiveTick()
{
    if (!m_live)
        return;
    refreshText();
    const qint64 elapsed = qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_liveStartMs);
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
    refreshText();
}

void AcpTurnFoldRow::setDiffStat(const AcpDiffStat::Lines &stat)
{
    if (m_diffStat.added == stat.added && m_diffStat.removed == stat.removed)
        return;
    m_diffStat = stat;
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
    if (!m_diffStat.isEmpty())
        text += QStringLiteral(" · ") + AcpDiffStat::label(m_diffStat);
    return text;
}

// text() stays the whole plain label (size hint, accessibility); only the
// paint splits it so the counts take the diff colours.
void AcpTurnFoldRow::paintEvent(QPaintEvent *event)
{
    const QString stat = AcpDiffStat::label(m_diffStat);
    const QString full = text();
    const qsizetype at = stat.isEmpty() ? -1 : full.lastIndexOf(stat);
    if (at < 0) {
        QPushButton::paintEvent(event);
        return;
    }
    QStylePainter p(this);
    QStyleOptionButton opt;
    initStyleOption(&opt);
    opt.text.clear();
    p.drawControl(QStyle::CE_PushButton, opt); // background, hover, focus
    const QRect r = style()->subElementRect(QStyle::SE_PushButtonContents, &opt, this);
    // The stylesheet's muted / hover colours, which the cleared label no longer carries.
    const QColor base = palette().color((opt.state & QStyle::State_MouseOver) ? QPalette::Text
                                                                               : QPalette::PlaceholderText);
    const QFontMetrics fm(font());
    int x = r.left();
    auto run = [&](const QString &s, const QColor &color) {
        if (s.isEmpty() || x > r.right())
            return;
        p.setPen(color);
        p.drawText(QRect(x, r.top(), r.right() - x + 1, r.height()),
                   Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, s);
        x += fm.horizontalAdvance(s);
    };
    const qsizetype space = stat.indexOf(QLatin1Char(' '));
    run(full.left(at), base);
    run(stat.left(space), QColor(QLatin1String(AcpDiffStat::kAddedColor)));
    run(stat.mid(space), QColor(QLatin1String(AcpDiffStat::kRemovedColor)));
    run(full.mid(at + stat.size()), base);
}

void AcpTurnFoldRow::refreshText()
{
    if (m_live) {
        setText(label(qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_liveStartMs)));
        setToolTip(tr("Working…"));
        return;
    }
    if (!m_expandable) {
        setText(label(m_workedMs));
        setToolTip(QString());
        return;
    }
    setText(label(m_workedMs) + (m_expanded ? QStringLiteral(" ⌄") : QStringLiteral(" ›")));
    setToolTip(m_expanded ? tr("Hide tool calls") : tr("Show tool calls"));
}
