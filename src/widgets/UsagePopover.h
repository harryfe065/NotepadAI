#ifndef USAGE_POPOVER_H
#define USAGE_POPOVER_H

#include <QFrame>
#include <QVector>

#include "ai/ClaudeUsage.h"

class QTimer;
class QToolButton;
class QVariantAnimation;
class QVBoxLayout;
class UsageAccountSection;

// Frameless popup anchored under the usage button: header ("Usage", % used/left
// toggle, refresh), then one Claude section per account, each with one
// UsageWindowBar per window.
class UsagePopover : public QFrame
{
    Q_OBJECT

public:
    static constexpr int kWidth = 320;

    // Snapshot of one account, copied out of its ClaudeUsageService.
    struct Account {
        QString label;          // email, else the config folder name
        QString configDir;      // resolved folder, for the tooltip
        ai::claudeusage::Usage usage;
        ai::claudeusage::Health health;
        bool loaded = false;
        bool loading = false;
        qint64 nowMs = 0;
    };

    explicit UsagePopover(QWidget *parent = nullptr);

    void setData(QVector<Account> accounts);
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

    QToolButton *m_modeButton = nullptr;
    QToolButton *m_refreshButton = nullptr;
    QVBoxLayout *m_sectionsLayout = nullptr;
    QVariantAnimation *m_spin = nullptr;
    QTimer *m_staleTimer = nullptr;   // keeps "Updated 3m ago / Retry in 4m" current while open
    // Section i sits under divider i (divider 0 is hidden: the header already has one).
    QVector<UsageAccountSection *> m_sections;
    QVector<QWidget *> m_dividers;

    QVector<Account> m_accounts;
    ai::claudeusage::PercentMode m_mode = ai::claudeusage::PercentMode::Used;
};

#endif // USAGE_POPOVER_H
