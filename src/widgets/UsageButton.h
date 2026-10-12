#ifndef USAGE_BUTTON_H
#define USAGE_BUTTON_H

#include <QElapsedTimer>
#include <QEnterEvent>
#include <QTimer>
#include <QToolButton>

#include "ai/ClaudeUsage.h"

namespace ai { class ClaudeUsageService; }
class UsagePopover;

// Menu-bar chip "[Claude glyph] 26%": the Claude window closest to its limit, text tinted
// warning / danger from 80% / 95%. Hidden until the first response and while
// signed out. Click opens the UsagePopover.
class UsageButton : public QToolButton
{
    Q_OBJECT

public:
    explicit UsageButton(ai::ClaudeUsageService *service, QWidget *parent = nullptr);

    void setPercentMode(ai::claudeusage::PercentMode mode);

signals:
    void percentModeChanged(ai::claudeusage::PercentMode mode);

protected:
    void changeEvent(QEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void onServiceChanged();
    void togglePopover();
    void applyTone(ai::claudeusage::Tone tone);
    void rebuildIcon();
    void relayoutMenuBar();
    void pushDataToPopover();

    ai::ClaudeUsageService *m_service = nullptr;
    UsagePopover *m_popover = nullptr;
    QTimer m_hoverTimer;          // debounced refresh while the pointer rests on the chip
    QElapsedTimer m_lastClosed;   // swallows the click that dismissed the popup
    ai::claudeusage::PercentMode m_mode = ai::claudeusage::PercentMode::Used;
    ai::claudeusage::Tone m_tone = ai::claudeusage::Tone::Ok;
    bool m_applyingTone = false;
};

#endif // USAGE_BUTTON_H
