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

#include "TerminalAreaToolbar.h"

#include "DockBottomToolTab.h"
#include "TerminalDock.h"
#include "TerminalIcons.h"
#include "TerminalProcessStats.h"

#include "DockAreaTabBar.h"
#include "DockAreaTitleBar.h"
#include "DockAreaWidget.h"
#include "DockWidget.h"

#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QPainter>
#include <QToolButton>
#include <QToolTip>

TerminalAreaToolbar::TerminalAreaToolbar(ads::CDockAreaWidget *area)
    : QWidget(area->titleBar())
    , m_area(area)
{
    setObjectName(QStringLiteral("TerminalAreaToolbar"));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 4, 0);
    layout->setSpacing(4);

    m_stats = new QLabel(this);
    m_stats->setTextFormat(Qt::PlainText);
    // Muted chrome text per ui-dna.
    m_stats->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    m_stats->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    const QFontMetrics fm(m_stats->font());
    m_stats->setMinimumWidth(fm.horizontalAdvance(QStringLiteral("CPU 100%   RAM 1023 MB")));
    m_stats->installEventFilter(this);
    layout->addWidget(m_stats);
    layout->addSpacing(4);

    m_find = makeButton(tr("Find (Ctrl+Shift+F)"));
    m_clear = makeButton(tr("Clear (Ctrl+Shift+L)"));
    m_restart = makeButton(tr("Restart task"));
    m_new = makeButton(tr("New terminal"));
    m_maximize = makeButton(tr("Maximize"));
    for (QToolButton *b : {m_find, m_clear, m_restart, m_new, m_maximize}) {
        layout->addWidget(b);
    }
    connect(m_find, &QToolButton::clicked, this, [this]() { if (m_dock) emit findRequested(m_dock); });
    connect(m_clear, &QToolButton::clicked, this, [this]() { if (m_dock) emit clearRequested(m_dock); });
    connect(m_restart, &QToolButton::clicked, this, [this]() { if (m_dock) emit restartRequested(m_dock); });
    connect(m_new, &QToolButton::clicked, this, [this]() { emit newTerminalRequested(m_dock); });
    connect(m_maximize, &QToolButton::clicked, this, [this]() { emit maximizeRequested(this); });

    retint();

    // Sit just left of the area's own tab-list / close buttons.
    ads::CDockAreaTitleBar *titleBar = area->titleBar();
    int index = titleBar->indexOf(titleBar->button(ads::TitleBarButtonTabsMenu));
    if (index < 0) {
        index = titleBar->indexOf(titleBar->tabBar()) + 1;
    }
    titleBar->insertWidget(index, this);

    connect(area, &ads::CDockAreaWidget::currentChanged, this, &TerminalAreaToolbar::syncToCurrent);
    syncToCurrent();
}

ads::CDockAreaWidget *TerminalAreaToolbar::area() const
{
    return m_area.data();
}

TerminalDock *TerminalAreaToolbar::dock() const
{
    return m_dock.data();
}

TerminalAreaToolbar *TerminalAreaToolbar::find(ads::CDockAreaWidget *area)
{
    if (!area) return nullptr;
    return area->titleBar()->findChild<TerminalAreaToolbar *>(QStringLiteral("TerminalAreaToolbar"),
                                                               Qt::FindDirectChildrenOnly);
}

QToolButton *TerminalAreaToolbar::makeButton(const QString &toolTip)
{
    auto *b = new QToolButton(this);
    b->setAutoRaise(true);
    b->setToolTip(toolTip);
    b->setIconSize(QSize(14, 14));
    // Keyboard-reachable, but a click does not pull focus out of the terminal.
    b->setFocusPolicy(Qt::TabFocus);
    return b;
}

void TerminalAreaToolbar::syncToCurrent()
{
    ads::CDockWidget *current = m_area ? m_area->currentDockWidget() : nullptr;
    TerminalDock *dock = current ? qobject_cast<TerminalDock *>(current->widget()) : nullptr;
    if (dock != m_dock) {
        disconnect(m_statsConnection);
        disconnect(m_presentationConnection);
        m_dock = dock;
        if (dock) {
            m_statsConnection = connect(dock, &TerminalDock::statsChanged, this, &TerminalAreaToolbar::updateStats);
            m_presentationConnection = connect(dock, &TerminalDock::presentationChanged, this, [this]() {
                m_restart->setVisible(m_dock && m_dock->isTask());
            });
        }
        emit currentDockChanged(dock);
    }
    setVisible(dock != nullptr);
    if (dock) {
        m_restart->setVisible(dock->isTask());
        m_stats->setVisible(!dock->isRemoteSession());
        setMaximized(isToolAreaMaximized(m_area));
    }
    updateStats();
}

void TerminalAreaToolbar::setMaximized(bool maximized)
{
    m_maximized = maximized;
    m_maximize->setToolTip(maximized ? tr("Restore") : tr("Maximize"));
    retint();
}

bool TerminalAreaToolbar::cpuLabelUnderMouse() const
{
    return isVisible() && m_stats->isVisible() && m_stats->underMouse();
}

void TerminalAreaToolbar::updateStats()
{
    if (!m_dock || m_dock->isRemoteSession()) {
        m_stats->clear();
        return;
    }
    const TerminalProcessStats::TreeStats stats = m_dock->processStats();
    m_stats->setText(TerminalProcessStats::cpuLabel(stats.valid, stats.cpuPercent)
                     + QStringLiteral("   ")
                     + TerminalProcessStats::ramLabel(stats.valid, stats.rssBytes));

    // Per-core load as a live tooltip while the pointer rests on the readout.
    const QString label = m_dock->coreLoad();
    const bool visible = QToolTip::isVisible();
    const QString shown = visible ? QToolTip::text() : QString();
    const bool over = m_stats->underMouse();
    m_stats->setToolTip(label);
    if (!TerminalProcessStats::coreTipNeedsPush(visible, over, shown, label))
        return;
    if (label.isEmpty()) {
        QToolTip::hideText();
        return;
    }
    if (m_coreTipAnchor.isNull())
        m_coreTipAnchor = QCursor::pos();
    // Same anchor so Qt reuses the tip instead of jumping it to the cursor.
    QToolTip::showText(m_coreTipAnchor, label, m_stats, m_stats->rect());
}

bool TerminalAreaToolbar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_stats) {
        switch (event->type()) {
        case QEvent::Enter:
            emit cpuHoverChanged();
            break;
        case QEvent::Leave:
        case QEvent::Hide:
            m_coreTipAnchor = {};
            emit cpuHoverChanged();
            break;
        case QEvent::ToolTip:
            m_coreTipAnchor = static_cast<QHelpEvent *>(event)->globalPos();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TerminalAreaToolbar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        retint();
    }
}

void TerminalAreaToolbar::retint()
{
    const QColor c = palette().color(QPalette::ButtonText);
    m_find->setIcon(terminalTintedIcon(QStringLiteral(":/icons/search.svg"), c));
    m_clear->setIcon(terminalTintedIcon(QStringLiteral(":/icons/eraser.svg"), c));
    m_restart->setIcon(terminalTintedIcon(QStringLiteral(":/icons/refresh-cw.svg"), c));
    m_new->setIcon(terminalTintedIcon(QStringLiteral(":/icons/plus.svg"), c));
    m_maximize->setIcon(terminalTintedIcon(m_maximized ? QStringLiteral(":/icons/minimize-2.svg")
                                                       : QStringLiteral(":/icons/maximize-2.svg"), c));
}
