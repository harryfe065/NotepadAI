#include "UsageWindowBar.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

#include "UsageStyle.h"

namespace {
constexpr int kTrackHeight = 6;
constexpr int kTrackRadius = 2;
constexpr int kRowGap = 2;
constexpr int kPaceTickWidth = 2;
constexpr int kPaceTickOverhang = 2; // px above and below the track, inside kRowGap
static_assert(kPaceTickOverhang <= kRowGap);
} // namespace

using namespace ai::claudeusage;

UsageWindowBar::UsageWindowBar(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void UsageWindowBar::setWindow(const QString &label, const RateWindow &window, PercentMode mode, qint64 nowMs)
{
    m_label = label;
    m_used = std::clamp(window.usedPercent, 0.0, 100.0);
    m_tone = toneFor(window.usedPercent);
    m_percent = percentText(window.usedPercent, mode);
    m_pace = elapsedPercent(window, nowMs);

    const QString in = formatResetIn(window.resetsAtMs, nowMs);
    const QString at = formatResetAt(window.resetsAtMs, nowMs);
    m_reset = at.isEmpty() ? in : QStringLiteral("Resets %1 · %2").arg(in, at);

    setAccessibleName(QStringLiteral("%1 usage, %2").arg(m_label, m_percent));
    setToolTip(m_pace ? QStringLiteral("%1% of the window has elapsed").arg(qRound(*m_pace)) : QString());
    updateGeometry();
    update();
}

QSize UsageWindowBar::sizeHint() const
{
    const int line = fontMetrics().height();
    return {200, line * 2 + kTrackHeight + kRowGap * 2};
}

QSize UsageWindowBar::minimumSizeHint() const
{
    return sizeHint();
}

void UsageWindowBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QFontMetrics fm = fontMetrics();
    const int line = fm.height();
    const int w = width();
    const QPalette &pal = palette();
    const QColor muted = pal.color(QPalette::PlaceholderText);

    QColor percentColor = muted;
    QColor fillColor = pal.color(QPalette::Highlight);
    if (m_tone == Tone::Warning)
        percentColor = fillColor = usagestyle::warningColor();
    else if (m_tone == Tone::Danger)
        percentColor = fillColor = usagestyle::dangerColor();

    int y = 0;
    p.setPen(pal.color(QPalette::WindowText));
    p.drawText(QRect(0, y, w, line), Qt::AlignLeft | Qt::AlignVCenter, m_label);
    p.setPen(percentColor);
    p.drawText(QRect(0, y, w, line), Qt::AlignRight | Qt::AlignVCenter, m_percent);
    y += line + kRowGap;

    // Track (20% accent), usage fill, then the even-burn pace as a tick drawn
    // over both, so usage past the pace never hides it.
    const QRectF track(0, y, w, kTrackHeight);
    QPainterPath clip;
    clip.addRoundedRect(track, kTrackRadius, kTrackRadius);
    p.setPen(Qt::NoPen);

    QColor trackColor = pal.color(QPalette::Highlight);
    trackColor.setAlphaF(0.20f);
    p.fillPath(clip, trackColor);

    p.setClipPath(clip);
    QPainterPath fill;
    fill.addRoundedRect(QRectF(0, y, w * (m_used / 100.0), kTrackHeight), kTrackRadius, kTrackRadius);
    p.fillPath(fill, fillColor);
    p.setClipping(false);

    if (m_pace && w >= kPaceTickWidth) {
        // Whole-pixel x keeps the tick crisp; clamped so it stays fully visible at 0% / 100%.
        const int x = std::clamp(qRound(w * (*m_pace / 100.0)) - kPaceTickWidth / 2, 0, w - kPaceTickWidth);
        p.fillRect(QRect(x, y - kPaceTickOverhang, kPaceTickWidth, kTrackHeight + 2 * kPaceTickOverhang),
                   pal.color(QPalette::WindowText));
    }
    y += kTrackHeight + kRowGap;

    p.setPen(muted);
    p.drawText(QRect(0, y, w, line), Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(m_reset, Qt::ElideRight, w));
}
