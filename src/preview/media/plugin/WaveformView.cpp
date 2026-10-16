#include "WaveformView.h"

#include "MediaFormat.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kRulerHeight = 18;
constexpr double kBuckets = WaveformPeaks::kBucketsPerSecond;
constexpr double kMinPixelsPerBucket = 2.0;

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t), float(a.greenF() * (1 - t) + b.greenF() * t),
                            float(a.blueF() * (1 - t) + b.blueF() * t));
}

// Ruler step (seconds) giving labels at least `minPixels` apart.
double rulerStep(double pixelsPerSecond, double minPixels)
{
    static const double steps[] = {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
    for (double s : steps) {
        if (s * pixelsPerSecond >= minPixels)
            return s;
    }
    return 7200;
}

} // namespace

WaveformView::WaveformView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(tr("Click or drag to seek, wheel to zoom, Shift+wheel to scroll, double-click to fit"));
}

void WaveformView::clear()
{
    m_peaks.clear();
    m_complete = false;
    update();
}

void WaveformView::appendPeaks(const std::vector<WaveformPeaks::Peak> &peaks)
{
    if (peaks.empty())
        return;
    const double from = double(m_peaks.size()) / kBuckets;
    m_peaks.insert(m_peaks.end(), peaks.begin(), peaks.end());
    const double to = double(m_peaks.size()) / kBuckets;
    // Without a known duration the fitted view grows with every slice.
    if (m_duration <= 0.0 && m_viewLength <= 0.0) {
        update();
        emit viewChanged();
    } else {
        update(columnsRect(from, to));
    }
}

void WaveformView::setDuration(double seconds)
{
    if (std::abs(seconds - m_duration) < 1e-6)
        return;
    m_duration = std::max(0.0, seconds);
    update();
    emit viewChanged();
}

void WaveformView::setPosition(double seconds)
{
    if (seconds == m_position)
        return;
    const double old = m_position;
    m_position = seconds;
    // Follow playback when zoomed in: page forward once the playhead leaves the view.
    if (m_viewLength > 0.0 && (seconds > m_viewStart + m_viewLength || seconds < m_viewStart)) {
        setView(seconds - m_viewLength * 0.1, m_viewLength);
        return;
    }
    update(columnsRect(std::min(old, seconds), std::max(old, seconds)).adjusted(-2, 0, 2, 0));
}

void WaveformView::setLoop(double start, double end)
{
    m_loopStart = start;
    m_loopEnd = end > start ? end : start;
    update();
}

void WaveformView::setView(double start, double length)
{
    const double total = std::max(m_duration, double(m_peaks.size()) / kBuckets);
    const double minLength = std::max(0.01, width() / (kMinPixelsPerBucket * kBuckets));
    if (total <= 0.0 || length >= total) {
        m_viewStart = 0.0;
        m_viewLength = 0.0;
    } else {
        m_viewLength = std::clamp(length, minLength, total);
        m_viewStart = std::clamp(start, 0.0, total - m_viewLength);
    }
    update();
    emit viewChanged();
}

void WaveformView::fit()
{
    setView(0.0, 0.0);
}

double WaveformView::xToSeconds(double x) const
{
    const double total = std::max(m_duration, double(m_peaks.size()) / kBuckets);
    const double start = m_viewLength > 0.0 ? m_viewStart : 0.0;
    const double length = m_viewLength > 0.0 ? m_viewLength : total;
    return width() > 0 ? start + x / width() * length : 0.0;
}

double WaveformView::secondsToX(double s) const
{
    const double total = std::max(m_duration, double(m_peaks.size()) / kBuckets);
    const double start = m_viewLength > 0.0 ? m_viewStart : 0.0;
    const double length = m_viewLength > 0.0 ? m_viewLength : total;
    return length > 0.0 ? (s - start) / length * width() : 0.0;
}

QRect WaveformView::columnsRect(double fromSeconds, double toSeconds) const
{
    const int x0 = int(std::floor(secondsToX(fromSeconds)));
    const int x1 = int(std::ceil(secondsToX(toSeconds)));
    return QRect(QPoint(std::max(0, x0), 0), QPoint(std::min(width(), x1 + 1), height())).normalized();
}

void WaveformView::paintRuler(QPainter &p, const QRect &clip)
{
    const QRect ruler(0, 0, width(), kRulerHeight);
    if (!clip.intersects(ruler))
        return;
    const QColor text = palette().color(QPalette::PlaceholderText);
    p.setPen(text);
    const double x0 = clip.left() - 60, x1 = clip.right() + 60;    // labels overhang their tick
    const double s0 = xToSeconds(std::max(0.0, x0)), s1 = xToSeconds(x1);
    const double length = xToSeconds(width()) - xToSeconds(0);
    if (length <= 0.0)
        return;
    const double step = rulerStep(width() / length, 70.0);
    const bool showMs = step < 1.0;
    for (double t = std::floor(s0 / step) * step; t <= s1; t += step) {
        const int x = int(std::lround(secondsToX(t)));
        p.drawLine(x, kRulerHeight - 5, x, kRulerHeight - 1);
        const qint64 ms = qint64(std::llround(t * 1000));
        const QString label = showMs ? MediaFormat::preciseTimeText(ms) : MediaFormat::timeText(ms);
        p.drawText(QRect(x + 3, 0, 80, kRulerHeight - 2), Qt::AlignLeft | Qt::AlignVCenter, label);
    }
}

void WaveformView::paintEvent(QPaintEvent *event)
{
    const QRect clip = event->rect();
    QPainter p(this);
    const QPalette &pal = palette();
    const QColor base = pal.color(QPalette::Base);
    const QColor accent = pal.color(QPalette::Highlight);
    const QColor muted = mix(pal.color(QPalette::Text), base, 0.55);
    p.fillRect(clip, base);

    const int top = kRulerHeight;
    const int h = height() - top;
    const double mid = top + h / 2.0;
    const double half = std::max(1.0, h / 2.0 - 4.0);

    if (m_loopEnd > m_loopStart) {
        QColor band = accent;
        band.setAlpha(36);
        const QRect loop = columnsRect(m_loopStart, m_loopEnd).adjusted(0, top, 0, 0) & clip;
        p.fillRect(loop, band);
        p.setPen(accent);
        for (double edge : {m_loopStart, m_loopEnd}) {
            const int x = int(std::lround(secondsToX(edge)));
            if (x >= clip.left() - 1 && x <= clip.right() + 1)
                p.drawLine(x, top, x, height());
        }
    }

    p.setPen(mix(muted, base, 0.5));
    p.drawLine(QPointF(clip.left(), mid), QPointF(clip.right() + 1, mid));

    // One vertical line per pixel column, folded over the buckets it covers.
    const double playX = secondsToX(m_position);
    QVector<QLineF> played, ahead;
    played.reserve(clip.width());
    ahead.reserve(clip.width());
    const qsizetype count = qsizetype(m_peaks.size());
    for (int x = clip.left(); x <= clip.right(); ++x) {
        const qsizetype b0 = qsizetype(std::floor(xToSeconds(x) * kBuckets));
        const qsizetype b1 = std::max(b0 + 1, qsizetype(std::floor(xToSeconds(x + 1) * kBuckets)));
        if (b1 <= 0 || b0 >= count)
            continue;
        qint8 lo = 127, hi = -127;
        for (qsizetype b = std::max<qsizetype>(0, b0), end = std::min(b1, count); b < end; ++b) {
            lo = std::min(lo, m_peaks[size_t(b)].min);
            hi = std::max(hi, m_peaks[size_t(b)].max);
        }
        const double yTop = mid - hi / 127.0 * half;
        const double yBottom = std::max(yTop + 1.0, mid - lo / 127.0 * half);
        (x + 0.5 < playX ? played : ahead).append(QLineF(x + 0.5, yTop, x + 0.5, yBottom));
    }
    p.setPen(QPen(accent, 1.0));
    p.drawLines(played);
    p.setPen(QPen(muted, 1.0));
    p.drawLines(ahead);

    paintRuler(p, clip);

    if (playX >= clip.left() - 1 && playX <= clip.right() + 1) {
        p.setPen(QPen(pal.color(QPalette::Text), 1.0));
        p.drawLine(QPointF(playX, top), QPointF(playX, height()));
    }

    if (!m_complete && count > 0) {
        // Where decoding has got to.
        const double edge = secondsToX(double(count) / kBuckets);
        if (edge >= clip.left() && edge <= clip.right()) {
            p.setPen(QPen(muted, 1.0, Qt::DotLine));
            p.drawLine(QPointF(edge, top), QPointF(edge, height()));
        }
    }
}

void WaveformView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    emit seekRequested(std::max(0.0, xToSeconds(event->position().x())));
}

void WaveformView::mouseMoveEvent(QMouseEvent *event)
{
    const double s = std::max(0.0, xToSeconds(event->position().x()));
    if (m_dragging && (event->buttons() & Qt::LeftButton))
        emit seekRequested(s);
    else
        m_dragging = false;
    emit hoverChanged(s);
}

void WaveformView::mouseDoubleClickEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    fit();
}

void WaveformView::wheelEvent(QWheelEvent *event)
{
    const QPoint delta = event->angleDelta();
    const double total = std::max(m_duration, double(m_peaks.size()) / kBuckets);
    if (total <= 0.0)
        return;
    const double length = m_viewLength > 0.0 ? m_viewLength : total;
    const double start = m_viewLength > 0.0 ? m_viewStart : 0.0;
    if (delta.x() != 0 || (event->modifiers() & Qt::ShiftModifier)) {
        const int steps = delta.x() != 0 ? delta.x() : delta.y();
        setView(start - steps / 120.0 * length * 0.1, length);
    } else if (delta.y() != 0) {
        const double anchor = xToSeconds(event->position().x());
        const double factor = std::pow(1.25, -delta.y() / 120.0);
        const double newLength = length * factor;
        const double fraction = width() > 0 ? event->position().x() / width() : 0.5;
        setView(anchor - fraction * newLength, newLength);
    }
    event->accept();
}

void WaveformView::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    emit hoverChanged(-1.0);
}

void WaveformView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_viewLength > 0.0)
        setView(m_viewStart, m_viewLength);
}
