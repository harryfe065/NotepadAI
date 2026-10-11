#ifndef USAGE_POPOVER_H
#define USAGE_POPOVER_H

#include <QFrame>
#include <QVector>

#include "ai/ClaudeUsage.h"

class QLabel;
class QToolButton;
class QVariantAnimation;
class QVBoxLayout;
class UsageWindowBar;

// Frameless popup anchored under the usage button: header ("Usage", % used/left
// toggle, refresh), then the Claude section with one UsageWindowBar per window.
class UsagePopover : public QFrame
{
    Q_OBJECT

public:
    static constexpr int kWidth = 320;

    explicit UsagePopover(QWidget *parent = nullptr);

    void setData(const ai::claudeusage::Usage &usage, bool loaded, bool loading, qint64 nowMs);
    void setPercentMode(ai::claudeusage::PercentMode mode);
    // Right edge aligned to the anchor's right edge, 6px below it, kept on screen.
    void showBelow(QWidget *anchor);

signals:
    void refreshRequested();
    void percentModeToggleRequested();
    void closed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void rebuild();
    void applyChrome();
    void updateRefreshIcon(qreal angle);
    void setSpinning(bool on);

    QLabel *m_providerIcon = nullptr;
    QLabel *m_hint = nullptr;
    QToolButton *m_modeButton = nullptr;
    QToolButton *m_refreshButton = nullptr;
    QVBoxLayout *m_body = nullptr;
    QVariantAnimation *m_spin = nullptr;
    QVector<UsageWindowBar *> m_bars;

    ai::claudeusage::Usage m_usage;
    ai::claudeusage::PercentMode m_mode = ai::claudeusage::PercentMode::Used;
    qint64 m_nowMs = 0;
    bool m_loaded = false;
    bool m_loading = false;
};

#endif // USAGE_POPOVER_H
