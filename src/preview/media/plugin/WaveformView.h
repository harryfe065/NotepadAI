#ifndef WAVEFORMVIEW_H
#define WAVEFORMVIEW_H

#include "WaveformPeaks.h"

#include <QWidget>

#include <vector>

// The audio tab's waveform: min / max envelope, time ruler, loop band and
// playhead (add-media-preview D2).
//
// Paints only the columns inside the update rect, so moving the playhead
// repaints a strip a few pixels wide. Each column folds the buckets it covers
// (O(buckets per pixel)); a whole hour at full zoom-out is ~360 k bucket reads.
// The view range is in seconds; wheel zooms around the cursor, Shift+wheel
// and horizontal wheels pan, double-click fits.
class WaveformView : public QWidget
{
    Q_OBJECT

public:
    explicit WaveformView(QWidget *parent = nullptr);

    void clear();
    void appendPeaks(const std::vector<WaveformPeaks::Peak> &peaks);
    void setComplete(bool complete) { m_complete = complete; update(); }
    // Total length, from the player or the file header; the envelope may be shorter while decoding.
    void setDuration(double seconds);
    double duration() const { return m_duration; }
    void setPosition(double seconds);
    void setLoop(double start, double end);     // end <= start clears

    double viewStart() const { return m_viewStart; }
    double viewLength() const { return m_viewLength; }
    void setView(double start, double length);
    void fit();

    QSize sizeHint() const override { return {600, 200}; }
    QSize minimumSizeHint() const override { return {120, 80}; }

signals:
    void seekRequested(double seconds);
    void hoverChanged(double seconds);          // < 0: left the view
    void viewChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    double xToSeconds(double x) const;
    double secondsToX(double s) const;
    QRect columnsRect(double fromSeconds, double toSeconds) const;
    void paintRuler(QPainter &p, const QRect &clip);

    std::vector<WaveformPeaks::Peak> m_peaks;
    double m_duration = 0.0;
    double m_position = 0.0;
    double m_loopStart = 0.0, m_loopEnd = 0.0;
    double m_viewStart = 0.0, m_viewLength = 0.0;  // 0 length: fit
    bool m_complete = false;
    bool m_dragging = false;
};

#endif // WAVEFORMVIEW_H
