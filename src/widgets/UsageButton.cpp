#include "UsageButton.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QMenuBar>
#include <QResizeEvent>

#include <algorithm>

#include "UsagePopover.h"
#include "UsageStyle.h"
#include "ai/ClaudeUsageAccounts.h"
#include "ai/ClaudeUsageService.h"

using namespace ai::claudeusage;

namespace {
constexpr int kIconSize = 16;
constexpr int kDismissClickGuardMs = 250;
constexpr int kHoverRefreshDelayMs = 300;   // a pass-over must not hit the network
} // namespace

UsageButton::UsageButton(ai::ClaudeUsageAccounts *accounts, QWidget *parent)
    : QToolButton(parent)
    , m_accounts(accounts)
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
    connect(&m_hoverTimer, &QTimer::timeout, this, [this] { m_accounts->refresh(); });

    connect(this, &QToolButton::clicked, this, &UsageButton::togglePopover);
    connect(m_accounts, &ai::ClaudeUsageAccounts::changed, this, &UsageButton::onServiceChanged);
    connect(m_popover, &UsagePopover::closed, this, [this] { m_lastClosed.start(); });
    connect(m_popover, &UsagePopover::refreshRequested, this, [this] { m_accounts->refresh(true); });
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
    const bool show = m_accounts->buttonVisible();
    const bool wasVisible = !isHidden();
    const QString oldText = text();

    const auto &accounts = m_accounts->accounts();
    const bool several = accounts.size() > 1;
    QStringList percents;
    QStringList tips;
    Tone tone = Tone::Ok;
    for (const ai::ClaudeUsageService *account : accounts) {
        const auto headline = account->headline();
        if (!account->buttonVisible() || !headline)
            continue;
        const double used = headline->window.usedPercent;
        percents.append(QStringLiteral("%1%").arg(displayPercent(used, m_mode)));
        const QString stale = account->health().error.isEmpty() ? QString() : tr(" (stale)");
        tips.append(several ? tr("%1: %2 %3%4").arg(account->accountLabel(), headline->label,
                                                     percentText(used, m_mode), stale)
                            : tr("Usage: Claude %1 %2%3").arg(headline->label, percentText(used, m_mode), stale));
        tone = std::max(tone, toneFor(used));
    }

    setText(percents.join(QStringLiteral(" · ")));
    if (tips.isEmpty())
        setToolTip(tr("Usage"));
    else
        setToolTip(several ? tr("Usage: Claude") + QLatin1Char('\n') + tips.join(QLatin1Char('\n')) : tips.first());
    setAccessibleName(toolTip());
    applyTone(tone);
    setVisible(show);

    if (show != wasVisible || text() != oldText)
        relayoutMenuBar();

    if (m_popover->isVisible())
        pushDataToPopover();
}

// QMenuBar lays its corner widget out only when that widget is shown/hidden or
// the bar is resized, never when the chip's text (and so its sizeHint) changes.
// Without this nudge "5%" -> "23%" keeps the old width and paints as "...".
void UsageButton::relayoutMenuBar()
{
    for (QWidget *p = parentWidget(); p; p = p->parentWidget()) {
        if (auto *bar = qobject_cast<QMenuBar *>(p)) {
            QResizeEvent resize(bar->size(), bar->size());
            QCoreApplication::sendEvent(bar, &resize);
            return;
        }
    }
}

void UsageButton::pushDataToPopover()
{
    QVector<UsagePopover::Account> data;
    data.reserve(m_accounts->accounts().size());
    for (const ai::ClaudeUsageService *account : m_accounts->accounts()) {
        data.append(UsagePopover::Account{account->accountLabel(),
                                          QDir::toNativeSeparators(account->resolvedConfigDir()),
                                          account->usage(), account->health(), account->loaded(),
                                          account->loading(), account->nowMs()});
    }
    m_popover->setData(std::move(data));
}

void UsageButton::togglePopover()
{
    if (m_popover->isVisible())
        return;
    // The press that dismissed the popup also arrives here as a click.
    if (m_lastClosed.isValid() && m_lastClosed.elapsed() < kDismissClickGuardMs)
        return;

    pushDataToPopover();
    m_accounts->refresh();
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
