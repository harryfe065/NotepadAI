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

#include "DiffStickyHeader.h"

#include "DiffDocument.h"
#include "TerminalIcons.h"

#include <QEvent>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>

using DiffDoc::Style;

namespace {

bool isPathStyle(quint8 s)
{
    return s == quint8(Style::Dir) || s == quint8(Style::Name) || s == quint8(Style::NameStruck);
}

} // namespace

DiffStickyHeader::DiffStickyHeader(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);

    auto makeButton = [this](const QString &tip) {
        auto *b = new QToolButton(this);
        b->setAutoRaise(true);
        b->setToolTip(tip);
        b->setIconSize(QSize(14, 14));
        b->setFocusPolicy(Qt::TabFocus);
        b->setCursor(Qt::ArrowCursor);
        return b;
    };
    m_copy = makeButton(tr("Copy path"));
    m_open = makeButton(tr("Open file"));
    connect(m_copy, &QToolButton::clicked, this, &DiffStickyHeader::copyPathClicked);
    connect(m_open, &QToolButton::clicked, this, &DiffStickyHeader::openClicked);
    retint();
}

void DiffStickyHeader::setRow(const QByteArray &text, const QByteArray &styles)
{
    if (text == m_text && styles == m_styles) return;
    m_text = text;
    m_styles = styles;
    layoutRuns();
    update();
}

void DiffStickyHeader::setLooks(const Looks &looks, const QColor &background, const QColor &rule)
{
    m_looks = looks;
    m_background = background;
    m_rule = rule;
    retint();
    update();
}

void DiffStickyHeader::setMetrics(const QFont &font, int lineHeight, int textStart)
{
    if (font == m_font && lineHeight == m_lineHeight && textStart == m_textStart) return;
    m_font = font;
    m_boldFont = font;
    m_boldFont.setBold(true);
    m_lineHeight = lineHeight;
    m_textStart = textStart;
    layoutRuns();
    update();
}

void DiffStickyHeader::layoutRuns()
{
    m_runs.clear();
    m_chevronRect = {};
    m_pathRect = {};
    const QFontMetricsF fm(m_font);
    const QFontMetricsF bold(m_boldFont);
    qreal x = m_textStart;
    const qsizetype n = qMin(m_text.size(), m_styles.size());
    qsizetype start = 0;
    while (start < n) {
        const quint8 style = static_cast<quint8>(m_styles[start]);
        qsizetype end = start + 1;
        while (end < n && static_cast<quint8>(m_styles[end]) == style) ++end;
        Run run;
        run.text = QString::fromUtf8(m_text.constData() + start, end - start);
        run.style = style;
        run.x = x;
        run.width = (style < m_looks.size() && m_looks[style].bold ? bold : fm).horizontalAdvance(run.text);
        x += run.width;
        const QRectF rect(run.x, 0, run.width, m_lineHeight);
        if (style == quint8(Style::Chevron)) m_chevronRect = m_chevronRect.united(rect);
        if (isPathStyle(style)) m_pathRect = m_pathRect.united(rect);
        m_runs.append(std::move(run));
        start = end;
    }
}

void DiffStickyHeader::placeButtons()
{
    const QSize hint = m_copy->sizeHint();
    const int side = qMin(hint.height(), height());
    const int y = (height() - side) / 2;
    int x = width() - 8 - side;
    m_open->setGeometry(x, y, side, side);
    x -= 4 + side;
    m_copy->setGeometry(x, y, side, side);
}

void DiffStickyHeader::retint()
{
    const QColor color = m_looks[quint8(Style::HeaderPlain)].fg.isValid()
                             ? m_looks[quint8(Style::HeaderPlain)].fg
                             : palette().color(QPalette::ButtonText);
    m_copy->setIcon(terminalTintedIcon(QStringLiteral(":/icons/copy.svg"), color));
    m_open->setIcon(terminalTintedIcon(QStringLiteral(":/icons/external-link.svg"), color));
}

void DiffStickyHeader::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), m_background);
    const QFontMetricsF fm(m_font);
    const qreal baseline = (m_lineHeight - (fm.ascent() + fm.descent())) / 2 + fm.ascent();
    const qreal clipRight = m_copy->geometry().left() - 4;
    p.setClipRect(QRectF(0, 0, clipRight, height()));
    for (const Run &run : m_runs) {
        if (run.x >= clipRight) break;
        const StyleLook &look = run.style < m_looks.size() ? m_looks[run.style] : m_looks[0];
        QFont f = look.bold ? m_boldFont : m_font;
        if (look.strike) f.setStrikeOut(true);
        p.setFont(f);
        p.setPen(look.fg);
        p.drawText(QPointF(run.x, baseline), run.text);
    }
    p.setClipping(false);
    // Bottom edge, so the pinned row reads as lifted over the text below.
    p.fillRect(QRect(0, height() - 1, width(), 1), m_rule);
}

void DiffStickyHeader::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPointF pos = event->position();
    if (m_chevronRect.adjusted(-4, 0, 4, 0).contains(pos))
        emit chevronClicked();
    else if (m_pathRect.contains(pos))
        emit pathClicked();
    event->accept();
}

void DiffStickyHeader::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    const bool link = m_chevronRect.adjusted(-4, 0, 4, 0).contains(pos) || m_pathRect.contains(pos);
    setCursor(link ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(event);
}

void DiffStickyHeader::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange) retint();
    QWidget::changeEvent(event);
}

void DiffStickyHeader::resizeEvent(QResizeEvent *event)
{
    placeButtons();
    QWidget::resizeEvent(event);
}
