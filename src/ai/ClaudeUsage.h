#ifndef AI_CLAUDE_USAGE_H
#define AI_CLAUDE_USAGE_H

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QString>
#include <QVector>

#include <optional>

// Claude Code subscription usage (5-hour / weekly / Fable-weekly windows), read
// from `GET https://api.anthropic.com/api/oauth/usage` with the access token the
// Claude CLI already stored on disk. Everything here is pure (no I/O, no
// widgets) so the parsing and formatting rules are unit-testable.
namespace ai::claudeusage {

struct RateWindow {
    double usedPercent = 0.0;           // 0..100, clamped
    int windowMinutes = 0;
    std::optional<qint64> resetsAtMs;   // unix ms

    bool operator==(const RateWindow &o) const = default;
};

enum class Status { Ok, Unavailable, Error };

struct Usage {
    Status status = Status::Unavailable;
    QString error;
    std::optional<RateWindow> session;
    std::optional<RateWindow> weekly;
    std::optional<RateWindow> fableWeekly;
};

enum class Tone { Ok, Warning, Danger };
enum class PercentMode { Used, Remaining };

enum class WindowKey { Session, Weekly, FableWeekly };

struct NamedWindow {
    WindowKey key = WindowKey::Session;
    QString label;
    RateWindow window;
};

constexpr double kWarningPercent = 80.0;
constexpr double kDangerPercent = 95.0;
constexpr int kSessionMinutes = 300;
constexpr int kWeeklyMinutes = 10080;

// Epoch seconds, epoch ms, numeric string or RFC 3339 -> unix ms. 1e10 splits
// seconds (< year 2286) from ms (> 1970-04).
std::optional<qint64> parseTimestampMs(const QJsonValue &value);

// `{"claudeAiOauth":{"accessToken":"..."}}` -> trimmed token, empty when absent.
QString parseCredentialsJson(const QByteArray &raw);

// `$CLAUDE_CONFIG_DIR/.credentials.json`, else `<home>/.claude/.credentials.json`.
QString credentialsPath(const QString &configDirEnv, const QString &homeDir);

Usage parseUsage(const QJsonObject &body);

// 401/403 expired sign-in, 429 rate limited, anything else "HTTP <n>".
QString httpErrorMessage(int httpStatus);

Tone toneFor(double usedPercent);
int displayPercent(double usedPercent, PercentMode mode);
// Share of the window already elapsed (0..100); nullopt when unknown.
std::optional<double> elapsedPercent(const RateWindow &window, qint64 nowMs);

// "resetting now", "in 45m", "in 2h 15m", "in 3d 4h"; "reset time unknown" when null.
QString formatResetIn(std::optional<qint64> resetsAtMs, qint64 nowMs);
// Local clock time of the reset: "2:30 PM" same day, otherwise "Thu 2:30 PM".
QString formatResetAt(std::optional<qint64> resetsAtMs, qint64 nowMs, const QLocale &locale = QLocale());
// "26% used" / "74% left".
QString percentText(double usedPercent, PercentMode mode);

// Windows the provider reported, in display order (5-hour, weekly, Fable weekly).
QVector<NamedWindow> windowsOf(const Usage &usage);
// The most-consumed window - the one that will stall an agent first.
std::optional<NamedWindow> tightestWindow(const Usage &usage);

} // namespace ai::claudeusage

#endif // AI_CLAUDE_USAGE_H
