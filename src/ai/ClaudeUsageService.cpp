#include "ClaudeUsageService.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRandomGenerator>

namespace ai {

namespace {
constexpr qint64 kMaxCredentialsBytes = 1 << 20;
// `.claude.json` carries the project history too; past this it is not parsed.
constexpr qint64 kMaxAccountFileBytes = 16 << 20;
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
    const QByteArray retryAfter = r->rawHeader("Retry-After");
    r->deleteLater();

    // Deliberately no logging: errorString() embeds the request URL and the
    // request carries a bearer token.
    if (status == 0)
        emit failed();
    else
        emit finished(status, body,
                      claudeusage::parseRetryAfter(retryAfter, QDateTime::currentMSecsSinceEpoch()).value_or(-1));
}

// --------------------------------------------------------------- service

QUrl ClaudeUsageService::usageUrl()
{
    return QUrl(QStringLiteral("https://api.anthropic.com/api/oauth/usage"));
}

ClaudeUsageService::ClaudeUsageService(IClaudeUsageTransport *transport,
                                       const QString &configDir,
                                       QObject *parent)
    : QObject(parent)
    , m_transport(transport)
    , m_configDir(configDir)
    , m_clock([] { return QDateTime::currentMSecsSinceEpoch(); })
    , m_jitter([] { return QRandomGenerator::global()->generateDouble(); })
{
    const QString dir = configDir.isEmpty() ? qEnvironmentVariable("CLAUDE_CONFIG_DIR") : configDir;
    const QString home = QDir::homePath();
    m_credentialsFile = claudeusage::credentialsPath(dir, home);
    m_accountFile = claudeusage::accountJsonPath(dir, home);
    m_folderLabel = claudeusage::configFolderLabel(dir, home);

    connect(m_transport, &IClaudeUsageTransport::finished, this, &ClaudeUsageService::onResponse);
    connect(m_transport, &IClaudeUsageTransport::failed, this, &ClaudeUsageService::onTransportFailed);

    // Coming back to the app after a while: don't wait for the next poll tick
    // (the gate in refresh() still applies).
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

QString ClaudeUsageService::resolvedConfigDir() const
{
    return QFileInfo(m_credentialsFile).path();
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
    const qint64 now = m_clock();
    if (m_health.retryAtMs > now)   // cooldown: not even an explicit click gets through
        return;
    if (!force && m_lastCompletedMs != 0 && now - m_lastCompletedMs < kMinRefreshGapMs)
        return;

    m_inflight = true;
    readAccountEmail();
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

void ClaudeUsageService::readAccountEmail()
{
    const QFileInfo info(m_accountFile);
    if (!info.exists()) {
        m_email.clear();
        m_accountFileMtimeMs = m_accountFileSize = -1;
        return;
    }
    const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
    const qint64 size = info.size();
    if (mtime == m_accountFileMtimeMs && size == m_accountFileSize)
        return;
    m_accountFileMtimeMs = mtime;
    m_accountFileSize = size;

    QFile f(m_accountFile);
    if (size <= kMaxAccountFileBytes && f.open(QIODevice::ReadOnly))
        m_email = claudeusage::parseAccountEmail(f.readAll());
    else
        m_email.clear();
}

void ClaudeUsageService::sendWithToken(const QString &token)
{
    m_transport->get({usageUrl(), token});
}

void ClaudeUsageService::publish()
{
    m_loaded = true;
    m_inflight = false;
    m_nowMs = m_clock();
    m_lastCompletedMs = m_nowMs;
    emit changed();
}

void ClaudeUsageService::succeed(claudeusage::Usage usage)
{
    m_usage = std::move(usage);
    m_health = {};
    m_health.lastOkMs = m_clock();
    publish();
}

void ClaudeUsageService::signedOut()
{
    m_usage = {};
    m_usage.status = claudeusage::Status::Unavailable;
    m_usage.error = QStringLiteral("Not signed in");
    m_health = {};
    publish();
}

void ClaudeUsageService::fail(const QString &error, qint64 retryAfterMs)
{
    using namespace claudeusage;
    // Numbers already on screen stay; only a first load has nothing to keep.
    if (!m_loaded || m_usage.status != Status::Ok) {
        m_usage = {};
        m_usage.status = Status::Error;
        m_usage.error = error;
    }
    const qint64 now = m_clock();
    m_health.error = error;
    m_health.failures += 1;
    m_health.retryAtMs = now + backoffDelayMs(m_health.failures,
                                              retryAfterMs >= 0 ? std::optional<qint64>(retryAfterMs) : std::nullopt,
                                              m_jitter());
    publish();
}

void ClaudeUsageService::onResponse(int httpStatus, const QByteArray &body, qint64 retryAfterMs)
{
    if (!m_inflight)
        return;

    using namespace claudeusage;
    if (httpStatus < 200 || httpStatus >= 300) {
        fail(httpErrorMessage(httpStatus), retryAfterMs);
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject())
        succeed(parseUsage(doc.object()));
    else
        fail(QStringLiteral("Unexpected usage response"), -1);
}

void ClaudeUsageService::onTransportFailed()
{
    if (!m_inflight)
        return;
    fail(QStringLiteral("Network error - check your connection"), -1);
}

void ClaudeUsageService::readKeychainAsync()
{
    auto unavailable = [this] { signedOut(); };

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
