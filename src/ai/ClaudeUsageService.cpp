#include "ClaudeUsageService.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>

namespace ai {

namespace {
constexpr qint64 kMaxCredentialsBytes = 1 << 20;
constexpr int kTransferTimeoutMs = 10'000;
} // namespace

// ------------------------------------------------------------- transport

ClaudeUsageHttpTransport::ClaudeUsageHttpTransport(QObject *parent)
    : IClaudeUsageTransport(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

ClaudeUsageHttpTransport::~ClaudeUsageHttpTransport()
{
    cancel();
}

void ClaudeUsageHttpTransport::get(const Request &req)
{
    cancel();

    QNetworkRequest http(req.url);
    http.setRawHeader("Authorization", "Bearer " + req.token.toUtf8());
    http.setRawHeader("anthropic-beta", "oauth-2025-04-20");
    http.setRawHeader("User-Agent", "claude-code/2.1.0");
    http.setRawHeader("Accept", "application/json");
    http.setTransferTimeout(kTransferTimeoutMs);

    m_reply = m_nam->get(http);
    connect(m_reply, &QNetworkReply::finished, this, &ClaudeUsageHttpTransport::onFinished);
}

void ClaudeUsageHttpTransport::cancel()
{
    if (!m_reply)
        return;
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    r->disconnect(this);
    r->abort();
    r->deleteLater();
}

void ClaudeUsageHttpTransport::onFinished()
{
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    if (!r)
        return;

    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = r->readAll();
    r->deleteLater();

    // Deliberately no logging: errorString() embeds the request URL and the
    // request carries a bearer token.
    if (status == 0)
        emit failed();
    else
        emit finished(status, body);
}

// --------------------------------------------------------------- service

QUrl ClaudeUsageService::usageUrl()
{
    return QUrl(QStringLiteral("https://api.anthropic.com/api/oauth/usage"));
}

ClaudeUsageService::ClaudeUsageService(IClaudeUsageTransport *transport,
                                       const QString &credentialsFile,
                                       QObject *parent)
    : QObject(parent)
    , m_transport(transport)
    , m_credentialsFile(credentialsFile.isEmpty()
                            ? claudeusage::credentialsPath(qEnvironmentVariable("CLAUDE_CONFIG_DIR"), QDir::homePath())
                            : credentialsFile)
    , m_clock([] { return QDateTime::currentMSecsSinceEpoch(); })
{
    connect(m_transport, &IClaudeUsageTransport::finished, this, &ClaudeUsageService::onResponse);
    connect(m_transport, &IClaudeUsageTransport::failed, this, &ClaudeUsageService::onTransportFailed);

    // Coming back to the app after a while: don't wait for the next 60 s tick
    // (the min-gap guard in refresh() still applies).
    if (qGuiApp) {
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) {
            if (s == Qt::ApplicationActive && m_pollTimer.isActive())
                refresh();
        });
    }

    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, [this] {
        if (QGuiApplication::applicationState() == Qt::ApplicationActive)
            refresh();
    });
}

bool ClaudeUsageService::buttonVisible() const
{
    return m_loaded && m_usage.status != claudeusage::Status::Unavailable;
}

void ClaudeUsageService::startPolling()
{
    if (!m_pollTimer.isActive())
        m_pollTimer.start();
    refresh();
}

void ClaudeUsageService::refresh(bool force)
{
    if (m_inflight)
        return;
    if (!force && m_lastCompletedMs != 0 && m_clock() - m_lastCompletedMs < kMinRefreshGapMs)
        return;

    m_inflight = true;
    emit changed();   // spinner on

    QString token;
    QFile f(m_credentialsFile);
    if (f.open(QIODevice::ReadOnly))
        token = claudeusage::parseCredentialsJson(f.read(kMaxCredentialsBytes));

    if (!token.isEmpty())
        sendWithToken(token);
    else
        readKeychainAsync();
}

void ClaudeUsageService::sendWithToken(const QString &token)
{
    m_transport->get({usageUrl(), token});
}

void ClaudeUsageService::complete(claudeusage::Usage usage)
{
    m_usage = std::move(usage);
    m_loaded = true;
    m_inflight = false;
    m_nowMs = m_clock();
    m_lastCompletedMs = m_nowMs;
    emit changed();
}

void ClaudeUsageService::onResponse(int httpStatus, const QByteArray &body)
{
    if (!m_inflight)
        return;

    using namespace claudeusage;
    Usage u;
    if (httpStatus < 200 || httpStatus >= 300) {
        u.status = Status::Error;
        u.error = httpErrorMessage(httpStatus);
    } else {
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            u = parseUsage(doc.object());
        } else {
            u.status = Status::Error;
            u.error = QStringLiteral("Unexpected usage response");
        }
    }
    complete(std::move(u));
}

void ClaudeUsageService::onTransportFailed()
{
    if (!m_inflight)
        return;
    claudeusage::Usage u;
    u.status = claudeusage::Status::Error;
    u.error = QStringLiteral("Network error - check your connection");
    complete(std::move(u));
}

void ClaudeUsageService::readKeychainAsync()
{
    auto unavailable = [this] {
        claudeusage::Usage u;
        u.status = claudeusage::Status::Unavailable;
        u.error = QStringLiteral("Not signed in");
        complete(std::move(u));
    };

#ifdef Q_OS_MACOS
    // macOS keeps Claude Code credentials in the login keychain, not a file.
    if (m_keychain) {
        unavailable();
        return;
    }
    m_keychain = new QProcess(this);
    connect(m_keychain, &QProcess::errorOccurred, this, [this, unavailable](QProcess::ProcessError) {
        if (!m_keychain)
            return;
        m_keychain->deleteLater();
        m_keychain = nullptr;
        unavailable();
    });
    connect(m_keychain, &QProcess::finished, this, [this, unavailable](int code, QProcess::ExitStatus) {
        if (!m_keychain)
            return;
        const QString token = code == 0 ? claudeusage::parseCredentialsJson(m_keychain->readAllStandardOutput()) : QString();
        m_keychain->deleteLater();
        m_keychain = nullptr;
        if (token.isEmpty())
            unavailable();
        else
            sendWithToken(token);
    });
    m_keychain->start(QStringLiteral("security"),
                      {QStringLiteral("find-generic-password"), QStringLiteral("-s"),
                       QStringLiteral("Claude Code-credentials"), QStringLiteral("-w")});
#else
    unavailable();
#endif
}

} // namespace ai
