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

#include "TerminalSearchBar.h"
#include "TerminalIcons.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QToolButton>

TerminalSearchBar::TerminalSearchBar(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("TerminalSearchBar"));
    setAutoFillBackground(true);
    setFrameShape(QFrame::NoFrame);
    // Chrome colours from the palette (ui-dna): a raised panel with a 1px rim.
    setStyleSheet(QStringLiteral(
        "#TerminalSearchBar { background: palette(window); border: 1px solid palette(mid); border-radius: 4px; }"));
    setCursor(Qt::ArrowCursor);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 4, 4);
    layout->setSpacing(4);

    m_edit = new QLineEdit(this);
    m_edit->setPlaceholderText(tr("Find"));
    m_edit->setClearButtonEnabled(false);
    m_edit->setFrame(false);
    m_edit->setMinimumWidth(QFontMetrics(m_edit->font()).horizontalAdvance(QLatin1Char('M')) * 18);
    m_edit->installEventFilter(this);
    layout->addWidget(m_edit, 1);

    m_case = makeToggle(QStringLiteral("Aa"), tr("Match case"));
    m_word = makeToggle(QStringLiteral("ab"), tr("Match whole word"));
    m_regex = makeToggle(QStringLiteral(".*"), tr("Use regular expression"));
    layout->addWidget(m_case);
    layout->addWidget(m_word);
    layout->addWidget(m_regex);

    m_count = new QLabel(this);
    m_count->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    m_count->setMinimumWidth(QFontMetrics(m_count->font()).horizontalAdvance(QStringLiteral("No results")));
    m_count->setAlignment(Qt::AlignCenter);
    layout->addSpacing(4);
    layout->addWidget(m_count);
    layout->addSpacing(4);

    m_prev = makeButton(tr("Previous match (Shift+Enter)"));
    m_next = makeButton(tr("Next match (Enter)"));
    m_close = makeButton(tr("Close (Esc)"));
    layout->addWidget(m_prev);
    layout->addWidget(m_next);
    layout->addWidget(m_close);

    connect(m_edit, &QLineEdit::textChanged, this, &TerminalSearchBar::optionsChanged);
    for (QToolButton *t : {m_case, m_word, m_regex}) {
        connect(t, &QToolButton::toggled, this, &TerminalSearchBar::optionsChanged);
    }
    connect(m_prev, &QToolButton::clicked, this, &TerminalSearchBar::previousRequested);
    connect(m_next, &QToolButton::clicked, this, &TerminalSearchBar::nextRequested);
    connect(m_close, &QToolButton::clicked, this, &TerminalSearchBar::closeRequested);

    retint();
    setCount(-1, 0);
    m_count->clear();
    adjustSize();
}

QToolButton *TerminalSearchBar::makeToggle(const QString &text, const QString &toolTip)
{
    auto *b = new QToolButton(this);
    b->setText(text);
    b->setToolTip(toolTip);
    b->setCheckable(true);
    b->setAutoRaise(true);
    // Reachable with Tab; clicking keeps the caret in the query.
    b->setFocusPolicy(Qt::TabFocus);
    QFont f = b->font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    b->setFont(f);
    return b;
}

QToolButton *TerminalSearchBar::makeButton(const QString &toolTip)
{
    auto *b = new QToolButton(this);
    b->setToolTip(toolTip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(14, 14));
    b->setFocusPolicy(Qt::TabFocus);
    return b;
}

TerminalSearchOptions TerminalSearchBar::options() const
{
    TerminalSearchOptions o;
    o.query = m_edit->text();
    o.caseSensitive = m_case->isChecked();
    o.regex = m_regex->isChecked();
    o.wholeWord = m_word->isChecked();
    return o;
}

void TerminalSearchBar::setQuery(const QString &query)
{
    if (query != m_edit->text()) {
        m_edit->setText(query);
    }
}

void TerminalSearchBar::focusQuery()
{
    m_edit->setFocus(Qt::ShortcutFocusReason);
    m_edit->selectAll();
}

void TerminalSearchBar::setCount(int active, int total)
{
    m_count->setToolTip(QString());
    if (m_edit->text().isEmpty()) {
        m_count->clear();
    } else if (total == 0) {
        m_count->setText(tr("No results"));
    } else {
        m_count->setText(QStringLiteral("%1/%2").arg(active + 1).arg(total));
    }
    m_prev->setEnabled(total > 0);
    m_next->setEnabled(total > 0);
}

void TerminalSearchBar::setInvalid(bool invalid)
{
    if (invalid) {
        m_count->setText(tr("Invalid"));
        m_count->setToolTip(QString());
    }
}

void TerminalSearchBar::setTooSlow()
{
    m_count->setText(tr("Too slow"));
    m_count->setToolTip(tr("This regular expression takes too long on this output. Simplify it to search."));
    m_prev->setEnabled(false);
    m_next->setEnabled(false);
}

bool TerminalSearchBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (ke->modifiers() & Qt::ShiftModifier) {
                emit previousRequested();
            } else {
                emit nextRequested();
            }
            return true;
        case Qt::Key_F3:
            if (ke->modifiers() & Qt::ShiftModifier) {
                emit previousRequested();
            } else {
                emit nextRequested();
            }
            return true;
        case Qt::Key_Escape:
            emit closeRequested();
            return true;
        default:
            break;
        }
    }
    if (watched == m_edit && event->type() == QEvent::ShortcutOverride) {
        // Keep Enter/Esc/F3 here instead of app shortcuts.
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape || ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter
            || ke->key() == Qt::Key_F3) {
            event->accept();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void TerminalSearchBar::changeEvent(QEvent *event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        retint();
    }
}

void TerminalSearchBar::retint()
{
    const QColor c = palette().color(QPalette::ButtonText);
    m_prev->setIcon(terminalTintedIcon(QStringLiteral(":/icons/chevron-up.svg"), c));
    m_next->setIcon(terminalTintedIcon(QStringLiteral(":/icons/chevron-down.svg"), c));
    m_close->setIcon(terminalTintedIcon(QStringLiteral(":/icons/cross.svg"), c));
}
