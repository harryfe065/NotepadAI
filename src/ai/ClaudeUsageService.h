#ifndef AI_CLAUDE_USAGE_SERVICE_H
#define AI_CLAUDE_USAGE_SERVICE_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <optional>

#include "ClaudeUsage.h"

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

namespace ai {

// One-shot GET against the usage endpoint. Seam so tests never touch the network.
class IClaudeUsageTransport : public QObject
{
    Q_OBJECT
public:
    struct Request {
        QUrl url;
        QString token;
    };

    explicit IClaudeUsageTransport(QObject *parent = nullptr) : QObject(parent) {}
    ~IClaudeUsageTransport() override = default;

    virtual void get(const Request &req) = 0;
    virtual void cancel() = 0;

signals:
    // Any HTTP response, success or not.
    void finished(int httpStatus, const QByteArray &body);
    // No HTTP response at all (DNS, TLS, timeout, cancelled-by-peer).
    void failed();
};

// QNetworkAccessManager transport: bearer token + the beta header the endpoint
// requires, 10 s transfer timeout. Never logs the token or the URL-bearing
// QNetworkReply::errorString().
class ClaudeUsageHttpTransport : public IClaudeUsageTransport
{
    Q_OBJECT
public:
    explicit ClaudeUsageHttpTransport(QObject *parent = nullptr);
    ~ClaudeUsageHttpTransport() override;

    void get(const Request &req) override;
    void cancel() override;

private:
    void onFinished();

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
};

// Owns the latest Claude usage snapshot. UI-thread object; the only blocking
// work is a sub-millisecond read of the small credentials file, everything
// else is async. At most one request in flight.
class ClaudeUsageService : public QObject
{
    Q_OBJECT
public:
    static constexpr int kPollIntervalMs = 60'000;
    // Non-forced refreshes closer together than this are dropped so opening the
    // popover repeatedly cannot trip the provider's rate limit.
    static constexpr qint64 kMinRefreshGapMs = 15'000;
    static QUrl usageUrl();

    // `credentialsFile` empty -> claudeusage::credentialsPath() from the environment.
    explicit ClaudeUsageService(IClaudeUsageTransport *transport,
                                const QString &credentialsFile = {},
                                QObject *parent = nullptr);

    const claudeusage::Usage &usage() const { return m_usage; }
    bool loaded() const { return m_loaded; }
    bool loading() const { return m_inflight; }
    // Wall clock (ms) of the last completed refresh; drives the countdowns.
    qint64 nowMs() const { return m_nowMs; }
    // Shown only once a response arrived and the user is signed in; an error
    // keeps it visible so the message is reachable.
    bool buttonVisible() const;
    std::optional<claudeusage::NamedWindow> headline() const { return claudeusage::tightestWindow(m_usage); }

    // `force` bypasses the min-gap guard (explicit refresh click); an in-flight
    // request is never duplicated either way.
    void refresh(bool force = false);
    // Starts the 60 s poll (ticks are skipped while the app is inactive).
    void startPolling();

    void setClock(std::function<qint64()> clock) { m_clock = std::move(clock); }

signals:
    void changed();

private:
    void sendWithToken(const QString &token);
    void complete(claudeusage::Usage usage);
    void onResponse(int httpStatus, const QByteArray &body);
    void onTransportFailed();
    void readKeychainAsync();

    IClaudeUsageTransport *m_transport = nullptr;
    QString m_credentialsFile;
    QTimer m_pollTimer;
    QProcess *m_keychain = nullptr;
    claudeusage::Usage m_usage;
    std::function<qint64()> m_clock;
    qint64 m_nowMs = 0;
    qint64 m_lastCompletedMs = 0;
    bool m_loaded = false;
    bool m_inflight = false;
};

} // namespace ai

#endif // AI_CLAUDE_USAGE_SERVICE_H
