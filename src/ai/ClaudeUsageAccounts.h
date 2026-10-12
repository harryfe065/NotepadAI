#ifndef AI_CLAUDE_USAGE_ACCOUNTS_H
#define AI_CLAUDE_USAGE_ACCOUNTS_H

#include <QObject>
#include <QStringList>
#include <QVector>

#include <functional>

namespace ai {

class ClaudeUsageService;
class IClaudeUsageTransport;

// The Claude accounts the usage chip tracks: one ClaudeUsageService per config
// folder, each with its own token, request and cooldown, so a rate-limited
// account never stalls the others. UI-thread object.
class ClaudeUsageAccounts : public QObject
{
    Q_OBJECT
public:
    // Makes the transport for a new account; the account takes ownership.
    using TransportFactory = std::function<IClaudeUsageTransport *()>;

    explicit ClaudeUsageAccounts(TransportFactory transportFactory, QObject *parent = nullptr);

    // Folders in display order (claudeusage::normalizeConfigDirs; empty list =
    // the default folder). A folder already tracked keeps its snapshot and
    // cooldown; a removed one is dropped with any request in flight; a new one
    // fetches right away once polling has started.
    void setConfigDirs(const QStringList &dirs);
    const QVector<ClaudeUsageService *> &accounts() const { return m_accounts; }

    // Any account signed in (or showing an error).
    bool buttonVisible() const;
    bool loading() const;
    // Fans out; every account applies its own min-gap and cooldown.
    void refresh(bool force = false);
    void startPolling();

    // Applied to every account, current and future; tests pin it.
    void setClock(std::function<qint64()> clock);

signals:
    // Any account changed, or the account list itself did.
    void changed();

private:
    ClaudeUsageService *createAccount(const QString &dir);

    TransportFactory m_transportFactory;
    QVector<ClaudeUsageService *> m_accounts;
    std::function<qint64()> m_clock;
    bool m_polling = false;
};

} // namespace ai

#endif // AI_CLAUDE_USAGE_ACCOUNTS_H
