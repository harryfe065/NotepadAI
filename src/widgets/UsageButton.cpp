#include "UsageButton.h"

#include <QEvent>

#include "UsagePopover.h"
#include "UsageStyle.h"
#include "ai/ClaudeUsageService.h"

using namespace ai::claudeusage;

namespace {
constexpr int kIconSize = 16;
constexpr int kDismissClickGuardMs = 250;
constexpr int kHoverRefreshDelayMs = 300;   // a pass-over must not hit the network
} // namespace

UsageButton::UsageButton(ai::ClaudeUsageService *service, QWidget *parent)
    : QToolButton(parent)
    , m_service(service)
    , m_popover(new UsagePopover(this))
{
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setIconSize({kIconSize, kIconSize});
    setFixedHeight(24);
    setAccessibleName(tr("Usage"));
    hide();   // until the first response says the user is signed in

    m_hoverTimer.setSingleShot(true);
    m_hoverTimer.setInterval(kHoverRefreshDelayMs);
    connect(&m_hoverTimer, &QTimer::timeout, this, [this] { m_service->refresh(); });

    connect(this, &QToolButton::clicked, this, &UsageButton::togglePopover);
    connect(m_service, &ai::ClaudeUsageService::changed, this, &UsageButton::onServiceChanged);
    connect(m_popover, &UsagePopover::closed, this, [this] { m_lastClosed.start(); });
    connect(m_popover, &UsagePopover::refreshRequested, this, [this] { m_service->refresh(true); });
    connect(m_popover, &UsagePopover::percentModeToggleRequested, this, [this] {
        const PercentMode next = m_mode == PercentMode::Used ? PercentMode::Remaining : PercentMode::Used;
        setPercentMode(next);
        emit percentModeChanged(next);
    });

    rebuildIcon();
    onServiceChanged();
}

void UsageButton::setPercentMode(PercentMode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    m_popover->setPercentMode(mode);
    onServiceChanged();
}

void UsageButton::onServiceChanged()
{
    const bool show = m_service->buttonVisible();
    setVisible(show);

    const auto headline = m_service->headline();
    if (headline) {
        const double used = headline->window.usedPercent;
        setText(QStringLiteral("%1%").arg(displayPercent(used, m_mode)));
        setToolTip(tr("Usage: Claude %1 %2").arg(headline->label, percentText(used, m_mode)));
        setAccessibleName(toolTip());
        applyTone(toneFor(used));
    } else {
        setText(QString());
        setToolTip(tr("Usage"));
        applyTone(Tone::Ok);
    }

    if (m_popover->isVisible())
        pushDataToPopover();
}

void UsageButton::pushDataToPopover()
{
    m_popover->setData(m_service->usage(), m_service->loaded(), m_service->loading(), m_service->nowMs());
}

void UsageButton::togglePopover()
{
    if (m_popover->isVisible())
        return;
    // The press that dismissed the popup also arrives here as a click.
    if (m_lastClosed.isValid() && m_lastClosed.elapsed() < kDismissClickGuardMs)
        return;

    pushDataToPopover();
    m_service->refresh();
    m_popover->showBelow(this);
}

void UsageButton::applyTone(Tone tone)
{
    if (tone == m_tone)
        return;
    m_tone = tone;

    // Text takes the tone color; Ok falls back to the inherited palette.
    m_applyingTone = true;
    if (tone == Tone::Ok) {
        setPalette(QPalette());
    } else {
        const QColor c = tone == Tone::Danger ? usagestyle::dangerColor() : usagestyle::warningColor();
        QPalette p = palette();
        p.setColor(QPalette::ButtonText, c);
        p.setColor(QPalette::WindowText, c);
        setPalette(p);
    }
    m_applyingTone = false;
    rebuildIcon();
}

void UsageButton::rebuildIcon()
{
    // Fixed brand color, so no palette tint and no tone color on the glyph.
    setIcon(QIcon(usagestyle::brandPixmap(QStringLiteral(":/icons/claude.svg"), kIconSize, devicePixelRatioF())));
}

void UsageButton::enterEvent(QEnterEvent *event)
{
    QToolButton::enterEvent(event);
    if (!m_popover->isVisible())
        m_hoverTimer.start();
}

void UsageButton::leaveEvent(QEvent *event)
{
    QToolButton::leaveEvent(event);
    m_hoverTimer.stop();
}

void UsageButton::changeEvent(QEvent *event)
{
    QToolButton::changeEvent(event);
    if (m_applyingTone)
        return;
    switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
    case QEvent::ApplicationPaletteChange:
        rebuildIcon();
        break;
    default:
        break;
    }
}
