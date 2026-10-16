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

#include "AiDockTitleBar.h"

#include <QDockWidget>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QToolButton>

namespace {

constexpr int kSpacingPx = 4;        // ui-dna sibling spacing
constexpr int kMaxNameWidthDip = 160; // a long agent name never crowds the title

QToolButton *makeChromeButton(QWidget *parent)
{
    auto *btn = new QToolButton(parent);
    btn->setAutoRaise(true);
    btn->setFocusPolicy(Qt::TabFocus);
    btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return btn;
}

} // namespace

AiDockTitleBar::AiDockTitleBar(QDockWidget *dock)
    : QWidget(dock)
    , m_dock(dock)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 1, 2, 1);
    layout->setSpacing(kSpacingPx);

    m_title = new QLabel(this);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // Presses on the title go to the dock: drag to move, double-click to float.
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);

    m_agentButton = makeChromeButton(this);
    m_agentButton->setObjectName(QLatin1String(kAgentButtonName));
    m_agentButton->setCursor(Qt::PointingHandCursor);
    m_agentButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_agentButton->hide();
    connect(m_agentButton, &QToolButton::clicked, this, [this]() {
        emit agentMenuRequested(m_agentButton->mapToGlobal(QPoint(0, m_agentButton->height())));
    });

    m_floatButton = makeChromeButton(this);
    m_floatButton->setToolTip(tr("Float"));
    m_floatButton->setAccessibleName(tr("Float"));
    connect(m_floatButton, &QToolButton::clicked, this, [this]() {
        if (m_dock)
            m_dock->setFloating(!m_dock->isFloating());
    });

    m_closeButton = makeChromeButton(this);
    m_closeButton->setToolTip(tr("Close"));
    m_closeButton->setAccessibleName(tr("Close"));
    connect(m_closeButton, &QToolButton::clicked, this, [this]() {
        if (m_dock)
            m_dock->close();
    });

    layout->addWidget(m_title, 1);
    layout->addWidget(m_agentButton);
    layout->addWidget(m_floatButton);
    layout->addWidget(m_closeButton);

    if (m_dock) {
        m_title->setText(m_dock->windowTitle());
        connect(m_dock, &QWidget::windowTitleChanged, m_title, &QLabel::setText);
        connect(m_dock, &QDockWidget::featuresChanged, this, [this]() { refreshDockButtons(); });
    }
    refreshDockButtons();
}

void AiDockTitleBar::setAgentButton(const AgentButton &agent)
{
    if (agent.name == m_agent.name && agent.tooltip == m_agent.tooltip
        && agent.switchingTo == m_agent.switchingTo
        && agent.icon.cacheKey() == m_agent.icon.cacheKey()) {
        return;
    }
    m_agent = agent;
    restyleAgentButton();
}

QString AiDockTitleBar::agentButtonText() const
{
    return m_agentButton->text();
}

QSize AiDockTitleBar::sizeHint() const
{
    // Never narrower than the buttons; the title elides into what is left.
    QSize sh = QWidget::sizeHint();
    sh.setHeight(qMax(sh.height(), style()->pixelMetric(QStyle::PM_TitleBarHeight, nullptr, this)));
    return sh;
}

QSize AiDockTitleBar::minimumSizeHint() const
{
    return QSize(0, sizeHint().height());
}

void AiDockTitleBar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        refreshDockButtons();
        restyleAgentButton();
    }
}

void AiDockTitleBar::mouseReleaseEvent(QMouseEvent *event)
{
    // The native title bar closes on a middle click (DockMiddleClickCloser);
    // with a custom bar the click lands here instead.
    if (event->button() == Qt::MiddleButton && m_dock
        && m_dock->features().testFlag(QDockWidget::DockWidgetClosable)) {
        m_dock->close();
        event->accept();
        return;
    }
    event->ignore();
}

void AiDockTitleBar::restyleAgentButton()
{
    if (m_agent.name.isEmpty()) {
        m_agentButton->hide();
        return;
    }
    const QString label = m_agent.switchingTo.isEmpty()
        ? m_agent.name
        : QStringLiteral("→ ") + m_agent.switchingTo + QStringLiteral("…");
    const QString elided = m_agentButton->fontMetrics().elidedText(label, Qt::ElideRight, kMaxNameWidthDip);
    m_agentButton->setText(elided + QStringLiteral(" ▾"));
    m_agentButton->setIcon(m_agent.icon);
    const int iconPx = m_agentButton->fontMetrics().height();
    m_agentButton->setIconSize(QSize(iconPx, iconPx));
    m_agentButton->setToolTip(m_agent.tooltip);
    m_agentButton->setAccessibleName(m_agent.name);
    // Toolbar chrome (ui-dna): auto-raise, 1px palette border on hover only.
    m_agentButton->setStyleSheet(
        QStringLiteral("QToolButton { color: palette(button-text); padding: 0px %1px; "
                       "border: 1px solid transparent; background: transparent; }"
                       "QToolButton:hover { background: palette(midlight); "
                       "border: 1px solid palette(mid); }")
            .arg(kSpacingPx));
    m_agentButton->show();
}

void AiDockTitleBar::refreshDockButtons()
{
    // The same glyphs Qt's own dock title bar draws.
    const int iconPx = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    m_floatButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton, nullptr, this));
    m_closeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton, nullptr, this));
    m_floatButton->setIconSize(QSize(iconPx, iconPx));
    m_closeButton->setIconSize(QSize(iconPx, iconPx));
    const QDockWidget::DockWidgetFeatures features =
        m_dock ? m_dock->features() : QDockWidget::DockWidgetFeatures();
    m_floatButton->setVisible(features.testFlag(QDockWidget::DockWidgetFloatable));
    m_closeButton->setVisible(features.testFlag(QDockWidget::DockWidgetClosable));
}
