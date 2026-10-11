#ifndef USAGE_WINDOW_BAR_H
#define USAGE_WINDOW_BAR_H

#include <QWidget>

#include <optional>

#include "ai/ClaudeUsage.h"

// One rate-limit row: "label ........ 26% used", a 6px track (even-burn pace
// underlay + usage fill) and "Resets in 2h 46m · Mon 12:10 AM". Painted in one
// pass from the live palette, so a theme switch needs no refresh logic.
class UsageWindowBar : public QWidget
{
    Q_OBJECT

public:
    explicit UsageWindowBar(QWidget *parent = nullptr);

    void setWindow(const QString &label, const ai::claudeusage::RateWindow &window,
                   ai::claudeusage::PercentMode mode, qint64 nowMs);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_label;
    QString m_percent;
    QString m_reset;
    double m_used = 0.0;
    std::optional<double> m_pace;
    ai::claudeusage::Tone m_tone = ai::claudeusage::Tone::Ok;
};

#endif // USAGE_WINDOW_BAR_H
