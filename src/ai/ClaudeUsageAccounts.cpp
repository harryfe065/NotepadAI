#include "ClaudeUsageAccounts.h"

#include <algorithm>

#include "ClaudeUsage.h"
#include "ClaudeUsageService.h"

namespace ai {

ClaudeUsageAccounts::ClaudeUsageAccounts(TransportFactory transportFactory, QObject *parent)
    : QObject(parent)
    , m_transportFactory(std::move(transportFactory))
{
}

ClaudeUsageService *ClaudeUsageAccounts::createAccount(const QString &dir)
{
    IClaudeUsageTransport *transport = m_transportFactory();
    auto *account = new ClaudeUsageService(transport, dir, this);
    transport->setParent(account);
    if (m_clock)
        account->setClock(m_clock);
    connect(account, &ClaudeUsageService::changed, this, &ClaudeUsageAccounts::changed);
    return account;
}

void ClaudeUsageAccounts::setConfigDirs(const QStringList &dirs)
{
    const QStringList keys = claudeusage::normalizeConfigDirs(dirs);

    QVector<ClaudeUsageService *> unclaimed = m_accounts;
    QVector<ClaudeUsageService *> next;
    QVector<ClaudeUsageService *> added;
    next.reserve(keys.size());
    for (const QString &key : keys) {
        // A handful of accounts: a linear scan beats hashing here.
        auto it = std::find_if(unclaimed.begin(), unclaimed.end(),
                               [&key](const ClaudeUsageService *a) { return a && a->configDir() == key; });
        if (it != unclaimed.end()) {
            next.append(*it);
            *it = nullptr;
        } else {
            next.append(createAccount(key));
            added.append(next.last());
        }
    }
    if (next == m_accounts)
        return;   // same folders, same order: keep everything, no repaint

    for (ClaudeUsageService *gone : std::as_const(unclaimed)) {
        if (!gone)
            continue;
        gone->disconnect(this);
        gone->deleteLater();   // its transport (a child) cancels on destruction
    }
    m_accounts = std::move(next);

    if (m_polling) {
        for (ClaudeUsageService *a : std::as_const(added))
            a->startPolling();
    }
    emit changed();
}

bool ClaudeUsageAccounts::buttonVisible() const
{
    return std::any_of(m_accounts.cbegin(), m_accounts.cend(),
                       [](const ClaudeUsageService *a) { return a->buttonVisible(); });
}

bool ClaudeUsageAccounts::loading() const
{
    return std::any_of(m_accounts.cbegin(), m_accounts.cend(),
                       [](const ClaudeUsageService *a) { return a->loading(); });
}

void ClaudeUsageAccounts::refresh(bool force)
{
    for (ClaudeUsageService *a : std::as_const(m_accounts))
        a->refresh(force);
}

void ClaudeUsageAccounts::startPolling()
{
    m_polling = true;
    for (ClaudeUsageService *a : std::as_const(m_accounts))
        a->startPolling();
}

void ClaudeUsageAccounts::setClock(std::function<qint64()> clock)
{
    m_clock = std::move(clock);
    for (ClaudeUsageService *a : std::as_const(m_accounts))
        a->setClock(m_clock);
}

} // namespace ai
