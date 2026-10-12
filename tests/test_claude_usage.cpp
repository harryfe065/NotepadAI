/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * SPDX short: GPL-3.0-or-later
 */

#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "ai/ClaudeUsage.h"
#include "ai/ClaudeUsageService.h"

using namespace ai;
using namespace ai::claudeusage;

namespace {

QJsonObject obj(const char *json)
{
    return QJsonDocument::fromJson(json).object();
}

qint64 localMs(int y, int mo, int d, int h, int mi)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi)).toMSecsSinceEpoch();
}

constexpr qint64 kMin = 60'000;

class FakeTransport : public IClaudeUsageTransport
{
public:
    void get(const Request &req) override { requests.append(req); }
    void cancel() override {}
    // retryAfterMs < 0 = the response carried no usable Retry-After.
    void reply(int status, const QByteArray &body, qint64 retryAfterMs = -1)
    {
        emit finished(status, body, retryAfterMs);
    }
    void fail() { emit failed(); }

    QList<Request> requests;
};

} // namespace

class TestClaudeUsage : public QObject
{
    Q_OBJECT

private slots:
    void timestamp_forms();
    void timestamp_rfc3339_with_microseconds_and_offset();
    void timestamp_rejects_garbage();
    void credentials_token_extraction();
    void credentials_path_env_then_home();
    void parse_maps_session_weekly_and_scoped_fable();
    void parse_clamps_and_accepts_used_percentage_alias();
    void parse_fable_fallback_keys();
    void parse_missing_windows_stay_empty();
    void tone_thresholds();
    void display_percent_modes_and_clamp();
    void elapsed_percent();
    void reset_in_boundaries();
    void reset_at_same_day_vs_other_day();
    void percent_text();
    void windows_order_and_tightest();
    void http_error_messages();

    void retry_after_delta_seconds();
    void retry_after_http_date();
    void retry_after_rejects_garbage();
    void backoff_doubles_floors_and_caps();
    void backoff_jitter_range();
    void backoff_server_retry_after_wins_only_when_larger();
    void format_ago_boundaries();
    void stale_notice_lines();
    void stale_notice_never_synced_shows_retry_time();

    void service_fetches_and_publishes();
    void service_unavailable_without_credentials_makes_no_request();
    void service_maps_http_and_parse_errors();
    void service_network_failure();
    void service_single_inflight();
    void service_min_gap_and_force();
    void service_button_visibility();
    void service_429_keeps_last_good_snapshot();
    void service_first_load_failure_is_error_with_cooldown();
    void service_retry_after_header_sets_cooldown();
    void service_backoff_doubles_on_repeated_failures();
    void service_force_blocked_during_cooldown();
    void service_success_clears_health_and_resets_backoff();
    void service_transport_failure_keeps_snapshot_and_backs_off();
    void service_unparsable_body_keeps_snapshot();
    void service_signed_out_replaces_snapshot();
    void service_min_gap_is_one_minute();
    void service_custom_config_dir_reads_that_dirs_token();
    void service_config_dir_change_drops_old_snapshot_and_refetches();
    void service_config_dir_change_clears_cooldown();
    void service_empty_config_dir_without_credentials_is_signed_out();
};

void TestClaudeUsage::timestamp_forms()
{
    QCOMPARE(parseTimestampMs(1'700'000'000), std::optional<qint64>(1'700'000'000'000));
    QCOMPARE(parseTimestampMs(qint64(1'700'000'000'000)), std::optional<qint64>(1'700'000'000'000));
    QCOMPARE(parseTimestampMs(QStringLiteral("1700000000")), std::optional<qint64>(1'700'000'000'000));
    QCOMPARE(parseTimestampMs(QStringLiteral("2023-11-14T22:13:20Z")), std::optional<qint64>(1'700'000'000'000));
}

void TestClaudeUsage::timestamp_rfc3339_with_microseconds_and_offset()
{
    QCOMPARE(parseTimestampMs(QStringLiteral("2023-11-14T22:13:20.123456+00:00")),
             std::optional<qint64>(1'700'000'000'123));
    QCOMPARE(parseTimestampMs(QStringLiteral("2023-11-15T05:13:20+07:00")),
             std::optional<qint64>(1'700'000'000'000));
}

void TestClaudeUsage::timestamp_rejects_garbage()
{
    QVERIFY(!parseTimestampMs(QStringLiteral("")));
    QVERIFY(!parseTimestampMs(QStringLiteral("   ")));
    QVERIFY(!parseTimestampMs(QStringLiteral("soon")));
    QVERIFY(!parseTimestampMs(QJsonValue(QJsonValue::Null)));
    QVERIFY(!parseTimestampMs(-5));
    QVERIFY(!parseTimestampMs(0));
    QVERIFY(!parseTimestampMs(QJsonValue(true)));
}

void TestClaudeUsage::credentials_token_extraction()
{
    QCOMPARE(parseCredentialsJson(R"({"claudeAiOauth":{"accessToken":" tok ","refreshToken":"r"}})"), QStringLiteral("tok"));
    QVERIFY(parseCredentialsJson("{}").isEmpty());
    QVERIFY(parseCredentialsJson("not json").isEmpty());
    QVERIFY(parseCredentialsJson(R"({"claudeAiOauth":{"accessToken":""}})").isEmpty());
    QVERIFY(parseCredentialsJson(R"({"claudeAiOauth":{"accessToken":5}})").isEmpty());
}

void TestClaudeUsage::credentials_path_env_then_home()
{
    QCOMPARE(credentialsPath(QStringLiteral("C:/cfg"), QStringLiteral("C:/home")),
             QStringLiteral("C:/cfg/.credentials.json"));
    QCOMPARE(credentialsPath(QString(), QStringLiteral("C:/home")),
             QStringLiteral("C:/home/.claude/.credentials.json"));
}

void TestClaudeUsage::parse_maps_session_weekly_and_scoped_fable()
{
    const Usage u = parseUsage(obj(R"({
        "five_hour": {"utilization": 42.5, "resets_at": "2023-11-14T22:13:20Z"},
        "seven_day": {"utilization": 130.0, "resets_at": 1700000000},
        "limits": [
            {"kind": "weekly_scoped", "percent": 12.0, "resets_at": 1700100000,
             "scope": {"model": {"display_name": " Fable "}}},
            {"kind": "weekly_scoped", "percent": 99.0,
             "scope": {"model": {"display_name": "Other"}}}
        ]})"));
    QCOMPARE(u.status, Status::Ok);
    QVERIFY(u.session && u.weekly && u.fableWeekly);
    QCOMPARE(u.session->usedPercent, 42.5);
    QCOMPARE(u.session->windowMinutes, 300);
    QCOMPARE(u.session->resetsAtMs, std::optional<qint64>(1'700'000'000'000));
    QCOMPARE(u.weekly->usedPercent, 100.0);
    QCOMPARE(u.weekly->windowMinutes, 10080);
    QCOMPARE(u.fableWeekly->usedPercent, 12.0);
    QCOMPARE(u.fableWeekly->resetsAtMs, std::optional<qint64>(1'700'100'000'000));
}

void TestClaudeUsage::parse_clamps_and_accepts_used_percentage_alias()
{
    const Usage u = parseUsage(obj(R"({
        "five_hour": {"used_percentage": -3},
        "seven_day": {"utilization": 250}})"));
    QCOMPARE(u.session->usedPercent, 0.0);
    QVERIFY(!u.session->resetsAtMs);
    QCOMPARE(u.weekly->usedPercent, 100.0);
}

void TestClaudeUsage::parse_fable_fallback_keys()
{
    for (const char *key : {"fable_weekly", "fable_seven_day", "seven_day_fable"}) {
        QJsonObject body;
        body.insert(QLatin1String(key), QJsonObject{{"utilization", 7.0}});
        const Usage u = parseUsage(body);
        QVERIFY2(u.fableWeekly.has_value(), key);
        QCOMPARE(u.fableWeekly->usedPercent, 7.0);
    }
}

void TestClaudeUsage::parse_missing_windows_stay_empty()
{
    const Usage u = parseUsage(QJsonObject());
    QCOMPARE(u.status, Status::Ok);
    QVERIFY(!u.session && !u.weekly && !u.fableWeekly);
}

void TestClaudeUsage::tone_thresholds()
{
    QCOMPARE(toneFor(0), Tone::Ok);
    QCOMPARE(toneFor(79.9), Tone::Ok);
    QCOMPARE(toneFor(80), Tone::Warning);
    QCOMPARE(toneFor(94.9), Tone::Warning);
    QCOMPARE(toneFor(95), Tone::Danger);
    QCOMPARE(toneFor(100), Tone::Danger);
}

void TestClaudeUsage::display_percent_modes_and_clamp()
{
    QCOMPARE(displayPercent(26.4, PercentMode::Used), 26);
    QCOMPARE(displayPercent(26.5, PercentMode::Used), 27);
    QCOMPARE(displayPercent(26.0, PercentMode::Remaining), 74);
    QCOMPARE(displayPercent(130.0, PercentMode::Used), 100);
    QCOMPARE(displayPercent(130.0, PercentMode::Remaining), 0);
    QCOMPARE(displayPercent(-4.0, PercentMode::Remaining), 100);
}

void TestClaudeUsage::elapsed_percent()
{
    const qint64 now = 1'000'000'000'000;
    const RateWindow w{10.0, 300, now + 150 * kMin};
    QCOMPARE(elapsedPercent(w, now).value(), 50.0);
    // Past the reset -> clamped to 100; far before the window -> clamped to 0.
    QCOMPARE(elapsedPercent(w, now + 400 * kMin).value(), 100.0);
    QCOMPARE(elapsedPercent(w, now - 1000 * kMin).value(), 0.0);
    QVERIFY(!elapsedPercent(RateWindow{10.0, 300, std::nullopt}, now));
    QVERIFY(!elapsedPercent(RateWindow{10.0, 0, now + kMin}, now));
}

void TestClaudeUsage::reset_in_boundaries()
{
    const qint64 now = 5'000'000'000;
    QCOMPARE(formatResetIn(std::nullopt, now), QStringLiteral("reset time unknown"));
    QCOMPARE(formatResetIn(now - kMin, now), QStringLiteral("resetting now"));
    QCOMPARE(formatResetIn(now + kMin - 1, now), QStringLiteral("resetting now"));
    QCOMPARE(formatResetIn(now + kMin, now), QStringLiteral("in 1m"));
    QCOMPARE(formatResetIn(now + 59 * kMin, now), QStringLiteral("in 59m"));
    QCOMPARE(formatResetIn(now + 60 * kMin, now), QStringLiteral("in 1h"));
    QCOMPARE(formatResetIn(now + (2 * 60 + 46) * kMin, now), QStringLiteral("in 2h 46m"));
    QCOMPARE(formatResetIn(now + 24 * 60 * kMin, now), QStringLiteral("in 1d"));
    QCOMPARE(formatResetIn(now + (2 * 24 + 2) * 60 * kMin, now), QStringLiteral("in 2d 2h"));
}

void TestClaudeUsage::reset_at_same_day_vs_other_day()
{
    const QLocale en(QLocale::English, QLocale::UnitedStates);
    const qint64 now = localMs(2026, 10, 8, 9, 0);   // Thursday
    // Same calendar day: clock time only (no NNBSP/NBSP before AM/PM).
    QCOMPARE(formatResetAt(localMs(2026, 10, 8, 14, 30), now, en), QStringLiteral("2:30 PM"));
    // Other day: short weekday + clock time.
    QCOMPARE(formatResetAt(localMs(2026, 10, 12, 0, 10), now, en), QStringLiteral("Mon 12:10 AM"));
    QCOMPARE(formatResetAt(localMs(2026, 10, 14, 0, 0), now, en), QStringLiteral("Wed 12:00 AM"));
    QVERIFY(formatResetAt(std::nullopt, now, en).isEmpty());
}

void TestClaudeUsage::percent_text()
{
    QCOMPARE(percentText(26.0, PercentMode::Used), QStringLiteral("26% used"));
    QCOMPARE(percentText(26.0, PercentMode::Remaining), QStringLiteral("74% left"));
}

void TestClaudeUsage::windows_order_and_tightest()
{
    Usage u;
    u.status = Status::Ok;
    u.fableWeekly = RateWindow{0.0, kWeeklyMinutes, std::nullopt};
    u.session = RateWindow{26.0, kSessionMinutes, std::nullopt};

    const auto list = windowsOf(u);
    QCOMPARE(list.size(), 2);
    QCOMPARE(list[0].label, QStringLiteral("5-hour"));
    QCOMPARE(list[1].label, QStringLiteral("Fable weekly"));
    QCOMPARE(tightestWindow(u)->key, WindowKey::Session);

    u.weekly = RateWindow{26.0, kWeeklyMinutes, std::nullopt};
    QCOMPARE(windowsOf(u)[1].label, QStringLiteral("Weekly"));
    // Ties keep the earlier (display-order) window.
    QCOMPARE(tightestWindow(u)->key, WindowKey::Session);

    QVERIFY(!tightestWindow(Usage{}));
    QVERIFY(windowsOf(Usage{}).isEmpty());
}

void TestClaudeUsage::http_error_messages()
{
    QVERIFY(httpErrorMessage(401).contains(QLatin1String("Sign-in expired")));
    QCOMPARE(httpErrorMessage(403), httpErrorMessage(401));
    QCOMPARE(httpErrorMessage(429), QStringLiteral("Rate limited by provider"));
    QCOMPARE(httpErrorMessage(500), QStringLiteral("Provider returned HTTP 500"));
}

// ---------------------------------------------------------- rate-limit math

void TestClaudeUsage::retry_after_delta_seconds()
{
    const qint64 now = 1'000'000;
    QCOMPARE(parseRetryAfter("120", now), std::optional<qint64>(120'000));
    QCOMPARE(parseRetryAfter("  7 ", now), std::optional<qint64>(7'000));
    QCOMPARE(parseRetryAfter("0", now), std::optional<qint64>(0));
}

void TestClaudeUsage::retry_after_http_date()
{
    // 1445412480 s = Wed, 21 Oct 2015 07:28:00 GMT.
    const qint64 at = 1'445'412'480'000;
    const QByteArray date = "Wed, 21 Oct 2015 07:28:00 GMT";
    QCOMPARE(parseRetryAfter(date, at - 90'000), std::optional<qint64>(90'000));
    // A date already in the past means "retry now", not a negative delay.
    QCOMPARE(parseRetryAfter(date, at + 5'000), std::optional<qint64>(0));
}

void TestClaudeUsage::retry_after_rejects_garbage()
{
    const qint64 now = 1'000'000;
    QVERIFY(!parseRetryAfter("", now));
    QVERIFY(!parseRetryAfter("soon", now));
    QVERIFY(!parseRetryAfter("-5", now));
    QVERIFY(!parseRetryAfter("12abc", now));
    QVERIFY(!parseRetryAfter("99999999999999999999", now));
}

void TestClaudeUsage::backoff_doubles_floors_and_caps()
{
    constexpr double mid = 0.5;   // jitter factor exactly 1.0
    QCOMPARE(backoffDelayMs(1, std::nullopt, mid), 60'000);
    QCOMPARE(backoffDelayMs(2, std::nullopt, mid), 120'000);
    QCOMPARE(backoffDelayMs(3, std::nullopt, mid), 240'000);
    QCOMPARE(backoffDelayMs(4, std::nullopt, mid), 480'000);
    QCOMPARE(backoffDelayMs(5, std::nullopt, mid), 900'000);    // 960 s capped to 15 min
    QCOMPARE(backoffDelayMs(100, std::nullopt, mid), 900'000);  // no shift overflow
    QCOMPARE(backoffDelayMs(0, std::nullopt, mid), 60'000);     // treated as the first failure
    QCOMPARE(backoffDelayMs(-3, std::nullopt, mid), 60'000);
}

void TestClaudeUsage::backoff_jitter_range()
{
    QCOMPARE(backoffDelayMs(2, std::nullopt, 0.0), 108'000);    // -10 %
    QCOMPARE(backoffDelayMs(2, std::nullopt, 1.0), 132'000);    // +10 %
    QCOMPARE(backoffDelayMs(1, std::nullopt, 0.0), 60'000);     // never below the 60 s floor
    QCOMPARE(backoffDelayMs(5, std::nullopt, 1.0), 900'000);    // never above the 15 min cap
}

void TestClaudeUsage::backoff_server_retry_after_wins_only_when_larger()
{
    constexpr double mid = 0.5;
    QCOMPARE(backoffDelayMs(1, 30'000, mid), 60'000);           // our floor is longer
    QCOMPARE(backoffDelayMs(2, 0, mid), 120'000);               // Retry-After: 0 never shortens
    QCOMPARE(backoffDelayMs(1, 600'000, mid), 600'000);         // server asks for more than the cap rule gives
    QCOMPARE(backoffDelayMs(5, 1'200'000, mid), 1'200'000);     // may exceed the 15 min self-imposed cap
    QCOMPARE(backoffDelayMs(1, 36'000'000, mid), 3'600'000);    // but never beyond 1 h
}

void TestClaudeUsage::format_ago_boundaries()
{
    const qint64 now = 5'000'000'000;
    QCOMPARE(formatAgo(now, now), QStringLiteral("just now"));
    QCOMPARE(formatAgo(now - 59'999, now), QStringLiteral("just now"));
    QCOMPARE(formatAgo(now - kMin, now), QStringLiteral("1m ago"));
    QCOMPARE(formatAgo(now - 59 * kMin, now), QStringLiteral("59m ago"));
    QCOMPARE(formatAgo(now - 60 * kMin, now), QStringLiteral("1h ago"));
    QCOMPARE(formatAgo(now - 125 * kMin, now), QStringLiteral("2h ago"));
    QCOMPARE(formatAgo(now - 24 * 60 * kMin, now), QStringLiteral("1d ago"));
    QCOMPARE(formatAgo(now + kMin, now), QStringLiteral("just now"));   // clock skew
}

void TestClaudeUsage::stale_notice_lines()
{
    const qint64 now = 5'000'000'000;
    Health h;
    h.error = QStringLiteral("Rate limited");
    h.lastOkMs = now - 3 * kMin;
    h.retryAtMs = now + 4 * kMin;
    // Old numbers are on screen: only say how old they are, no error, no countdown.
    QCOMPARE(staleNotice(h, now), QStringLiteral("Last synced 3m ago"));

    h.lastOkMs = now - 10'000;
    QCOMPARE(staleNotice(h, now), QStringLiteral("Last synced just now"));

    // Never synced and no cooldown known: nothing to show but the error.
    h.lastOkMs = 0;
    h.retryAtMs = 0;
    QCOMPARE(staleNotice(h, now), QStringLiteral("Rate limited"));
}

void TestClaudeUsage::stale_notice_never_synced_shows_retry_time()
{
    const QLocale en(QLocale::English, QLocale::UnitedStates);
    const qint64 now = localMs(2026, 10, 8, 9, 0);
    Health h;
    h.error = httpErrorMessage(429);
    h.retryAtMs = localMs(2026, 10, 8, 14, 30);
    QCOMPARE(staleNotice(h, now, en), QStringLiteral("Rate limited by provider\nTry again at 2:30 PM"));

    h.retryAtMs = localMs(2026, 10, 9, 0, 10);   // other day -> weekday prefix
    QCOMPARE(staleNotice(h, now, en), QStringLiteral("Rate limited by provider\nTry again at Fri 12:10 AM"));

    // Cooldown already over: the vague "shortly" is gone, so is the time.
    h.retryAtMs = now;
    QCOMPARE(staleNotice(h, now, en), QStringLiteral("Rate limited by provider"));

    // With a previous sync the age wins; the retry time is not shown.
    h.retryAtMs = localMs(2026, 10, 8, 14, 30);
    h.lastOkMs = now - 3 * kMin;
    QCOMPARE(staleNotice(h, now, en), QStringLiteral("Last synced 3m ago"));
}

// ---------------------------------------------------------------- service

namespace {

struct ServiceFixture {
    QTemporaryDir dir;
    FakeTransport transport;
    ClaudeUsageService *service = nullptr;
    qint64 clock = 1'000'000;

    explicit ServiceFixture(bool withCredentials = true)
    {
        const QString path = dir.filePath(QStringLiteral(".credentials.json"));
        if (withCredentials) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(R"({"claudeAiOauth":{"accessToken":"secret-token"}})");
        }
        service = new ClaudeUsageService(&transport, path, &transport);
        service->setClock([this] { return clock; });
        service->setJitter([] { return 0.5; });   // factor exactly 1.0: delays are the plain base
    }

    // Past any self-imposed cooldown (the longest is Retry-After's 1 h clamp).
    void waitOutCooldown() { clock += 3'600'000; }
};

const QByteArray kBody = R"({"five_hour":{"utilization":26.0,"resets_at":1700000000},
    "limits":[{"kind":"weekly_scoped","percent":0.0,"scope":{"model":{"display_name":"Fable"}}}]})";

} // namespace

void TestClaudeUsage::service_fetches_and_publishes()
{
    ServiceFixture fx;
    QSignalSpy changed(fx.service, &ClaudeUsageService::changed);

    fx.service->refresh();
    QCOMPARE(fx.transport.requests.size(), 1);
    QCOMPARE(fx.transport.requests[0].url, ClaudeUsageService::usageUrl());
    QCOMPARE(fx.transport.requests[0].token, QStringLiteral("secret-token"));
    QVERIFY(fx.service->loading());
    QVERIFY(!fx.service->loaded());

    fx.transport.reply(200, kBody);
    QVERIFY(!fx.service->loading());
    QVERIFY(fx.service->loaded());
    QCOMPARE(fx.service->usage().status, Status::Ok);
    QCOMPARE(fx.service->usage().session->usedPercent, 26.0);
    QVERIFY(fx.service->usage().fableWeekly.has_value());
    QCOMPARE(fx.service->nowMs(), fx.clock);
    QCOMPARE(fx.service->headline()->key, WindowKey::Session);
    QVERIFY(changed.count() >= 1);
}

void TestClaudeUsage::service_unavailable_without_credentials_makes_no_request()
{
    ServiceFixture fx(false);
    fx.service->refresh();
    QCOMPARE(fx.transport.requests.size(), 0);
    QVERIFY(fx.service->loaded());
    QCOMPARE(fx.service->usage().status, Status::Unavailable);
    QVERIFY(!fx.service->buttonVisible());
}

void TestClaudeUsage::service_maps_http_and_parse_errors()
{
    ServiceFixture fx;
    fx.service->refresh();
    fx.transport.reply(401, "{}");
    QCOMPARE(fx.service->usage().status, Status::Error);
    QCOMPARE(fx.service->usage().error, httpErrorMessage(401));

    fx.waitOutCooldown();
    fx.service->refresh();
    fx.transport.reply(200, "<html>");
    QCOMPARE(fx.service->usage().status, Status::Error);
    QCOMPARE(fx.service->usage().error, QStringLiteral("Unexpected usage response"));

    fx.waitOutCooldown();
    fx.service->refresh();
    fx.transport.reply(200, "[1,2]");
    QCOMPARE(fx.service->usage().error, QStringLiteral("Unexpected usage response"));
}

void TestClaudeUsage::service_network_failure()
{
    ServiceFixture fx;
    fx.service->refresh();
    fx.transport.fail();
    QVERIFY(fx.service->loaded());
    QCOMPARE(fx.service->usage().status, Status::Error);
    QVERIFY(fx.service->usage().error.startsWith(QLatin1String("Network error")));
    QVERIFY(!fx.service->loading());
}

void TestClaudeUsage::service_single_inflight()
{
    ServiceFixture fx;
    fx.service->refresh();
    fx.service->refresh(true);
    fx.service->refresh();
    QCOMPARE(fx.transport.requests.size(), 1);
}

void TestClaudeUsage::service_min_gap_and_force()
{
    ServiceFixture fx;
    fx.service->refresh();
    fx.transport.reply(200, kBody);

    fx.clock += ClaudeUsageService::kMinRefreshGapMs - 1;
    fx.service->refresh();                       // too soon
    QCOMPARE(fx.transport.requests.size(), 1);

    fx.service->refresh(true);                   // explicit click bypasses the gap
    QCOMPARE(fx.transport.requests.size(), 2);
    fx.transport.reply(200, kBody);

    fx.clock += ClaudeUsageService::kMinRefreshGapMs;
    fx.service->refresh();
    QCOMPARE(fx.transport.requests.size(), 3);
}

void TestClaudeUsage::service_button_visibility()
{
    ServiceFixture fx;
    QVERIFY(!fx.service->buttonVisible());       // nothing yet

    fx.service->refresh();
    fx.transport.reply(200, kBody);
    QVERIFY(fx.service->buttonVisible());

    fx.clock += ClaudeUsageService::kMinRefreshGapMs;
    fx.service->refresh();
    fx.transport.reply(429, "{}");               // error keeps the button reachable
    QVERIFY(fx.service->buttonVisible());
}

// ------------------------------------------------ rate limit: keep + back off

namespace {

// A service that already holds one good snapshot (26 % session), clock advanced
// past the min-gap so the next refresh() is allowed.
void primeWithGoodSnapshot(ServiceFixture &fx)
{
    fx.service->refresh();
    fx.transport.reply(200, kBody);
    fx.clock += ClaudeUsageService::kMinRefreshGapMs;
}

} // namespace

void TestClaudeUsage::service_429_keeps_last_good_snapshot()
{
    ServiceFixture fx;
    primeWithGoodSnapshot(fx);
    const qint64 okAt = fx.service->nowMs();

    fx.service->refresh();
    fx.transport.reply(429, "{}");

    QCOMPARE(fx.service->usage().status, Status::Ok);
    QCOMPARE(fx.service->usage().session->usedPercent, 26.0);
    QCOMPARE(fx.service->headline()->key, WindowKey::Session);
    QVERIFY(fx.service->buttonVisible());
    QCOMPARE(fx.service->health().error, httpErrorMessage(429));
    QCOMPARE(fx.service->health().lastOkMs, okAt);
    QCOMPARE(fx.service->health().failures, 1);
    QCOMPARE(fx.service->health().retryAtMs, fx.clock + 60'000);
    QVERIFY(!fx.service->loading());
}

void TestClaudeUsage::service_first_load_failure_is_error_with_cooldown()
{
    ServiceFixture fx;
    fx.service->refresh();
    fx.transport.reply(429, "{}");

    QCOMPARE(fx.service->usage().status, Status::Error);
    QCOMPARE(fx.service->usage().error, httpErrorMessage(429));
    QCOMPARE(fx.service->health().lastOkMs, qint64(0));
    QCOMPARE(fx.service->health().retryAtMs, fx.clock + 60'000);
}

void TestClaudeUsage::service_retry_after_header_sets_cooldown()
{
    ServiceFixture fx;
    primeWithGoodSnapshot(fx);

    fx.service->refresh();
    fx.transport.reply(429, "{}", 600'000);
    QCOMPARE(fx.service->health().retryAtMs, fx.clock + 600'000);

    // One ms before the server's deadline: still silent. At the deadline: goes out.
    fx.clock += 600'000 - 1;
    fx.service->refresh();
    QCOMPARE(fx.transport.requests.size(), 2);
    fx.clock += 1;
    fx.service->refresh();
    QCOMPARE(fx.transport.requests.size(), 3);
}

void TestClaudeUsage::service_backoff_doubles_on_repeated_failures()
{
    ServiceFixture fx;
    primeWithGoodSnapshot(fx);

    const qint64 expected[] = {60'000, 120'000, 240'000};
    for (int i = 0; i < 3; ++i) {
        fx.service->refresh();
        fx.transport.reply(429, "{}");
        QCOMPARE(fx.service->health().failures, i + 1);
        QCOMPARE(fx.service->health().retryAtMs, fx.clock + expected[i]);
        fx.clock += expected[i];
    }
    QCOMPARE(fx.transport.requests.size(), 4);   // 1 priming + 3 failures, nothing extra
}

void TestClaudeUsage::service_force_blocked_during_cooldown()
{
    ServiceFixture fx;
    primeWithGoodSnapshot(fx);
    fx.service->refresh();
    fx.transport.reply(429, "{}");
    QCOMPARE(fx.transport.requests.size(), 2);

    fx.clock += 59'999;
    fx.service->refresh(true);                   // the refresh button
    fx.service->refresh();
    QCOMPARE(fx.transport.requests.size(), 2);
    QVERIFY(!fx.service->loading());

    fx.clock += 1;
    fx.service->refresh(true);                   // cooldown over: force works again
    QCOMPARE(fx.transport.requests.size(), 3);
}

void TestClaudeUsage::service_success_clears_health_and_resets_backoff()
{
    ServiceFixture fx;
    primeWithGoodSnapshot(fx);
    for (int i = 0; i < 2; ++i) {
        fx.service->refresh();
        fx.transport.reply(429, "{}");
        fx.waitOutCooldown();
    }
    QCOMPARE(fx.service->health().failures, 2);

    fx.service->refresh();
    fx.transport.reply(200, kBody);
    QVERIFY(fx.service->health().error.isEmpty());
    QCOMPARE(fx.service->health().failures, 0);
    QCOMPARE(fx.service->health().retryAtMs, qint64(0));
    QCOMPARE(fx.service->health().lastOkMs, fx.clock);

    // The streak restarted: the next failure waits the base 60 s, not 240 s.
    fx.clock += ClaudeUsageService::kMinRefreshGapMs;
    fx.service->refresh();
    fx.transport.reply(500, "{}");
    QCOMPARE(fx.service->health().retryAtMs, fx.clock + 60'000);
}

void TestClaudeUsage::service_transport_failure_keeps_snapshot_and_backs_off()
{
    ServiceFixture fx;
    primeWithGoodSnapshot(fx);
    fx.service->refresh();
    fx.transport.fail();

    QCOMPARE(fx.service->usage().status, Status::Ok);
    QVERIFY(fx.service->health().error.startsWith(QLatin1String("Network error")));
    QCOMPARE(fx.service->health().retryAtMs, fx.clock + 60'000);
    QVERIFY(!fx.service->loading());
}

void TestClaudeUsage::service_unparsable_body_keeps_snapshot()
{
    ServiceFixture fx;
    primeWithGoodSnapshot(fx);
    fx.service->refresh();
    fx.transport.reply(200, "<html>");

    QCOMPARE(fx.service->usage().status, Status::Ok);
    QCOMPARE(fx.service->usage().session->usedPercent, 26.0);
    QCOMPARE(fx.service->health().error, QStringLiteral("Unexpected usage response"));
    QCOMPARE(fx.service->health().failures, 1);
}

void TestClaudeUsage::service_signed_out_replaces_snapshot()
{
    ServiceFixture fx;
    primeWithGoodSnapshot(fx);
    QVERIFY(QFile::remove(fx.dir.filePath(QStringLiteral(".credentials.json"))));

    fx.service->refresh();                       // no token -> no request at all
    QCOMPARE(fx.transport.requests.size(), 1);
    QCOMPARE(fx.service->usage().status, Status::Unavailable);
    QVERIFY(!fx.service->buttonVisible());
    QVERIFY(fx.service->health().error.isEmpty());
}

void TestClaudeUsage::service_min_gap_is_one_minute()
{
    ServiceFixture fx;
    fx.service->refresh();
    fx.transport.reply(200, kBody);

    // A burst of triggers (agent turn ends, popover opens, hover) costs nothing.
    fx.clock += 59'999;
    for (int i = 0; i < 50; ++i)
        fx.service->refresh();
    QCOMPARE(fx.transport.requests.size(), 1);

    fx.clock += 1;
    for (int i = 0; i < 50; ++i)
        fx.service->refresh();
    QCOMPARE(fx.transport.requests.size(), 2);   // one request, the rest see it in flight
}

// ------------------------------------------------------ custom config folder

namespace {

// A second Claude config dir (e.g. ~/.claude-sub) holding its own token.
struct OtherConfigDir {
    QTemporaryDir dir;
    explicit OtherConfigDir(const char *token)
    {
        QFile f(dir.filePath(QStringLiteral(".credentials.json")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(R"({"claudeAiOauth":{"accessToken":")") + token + R"("}})");
    }
    QString path() const { return dir.path(); }
};

} // namespace

void TestClaudeUsage::service_custom_config_dir_reads_that_dirs_token()
{
    ServiceFixture fx;
    OtherConfigDir other("sub-token");

    fx.service->setConfigDir(other.path());

    QCOMPARE(fx.transport.requests.size(), 1);
    QCOMPARE(fx.transport.requests[0].token, QStringLiteral("sub-token"));   // not "secret-token"
}

void TestClaudeUsage::service_config_dir_change_drops_old_snapshot_and_refetches()
{
    ServiceFixture fx;
    OtherConfigDir other("sub-token");
    primeWithGoodSnapshot(fx);

    fx.service->setConfigDir(other.path());

    // Another account: the old numbers must not linger, even stale.
    QVERIFY(!fx.service->loaded());
    QVERIFY(!fx.service->buttonVisible());
    QVERIFY(fx.service->loading());
    QCOMPARE(fx.transport.requests.size(), 2);

    fx.transport.reply(200, R"({"five_hour":{"utilization":70.0}})");
    QCOMPARE(fx.service->usage().session->usedPercent, 70.0);
    QVERIFY(fx.service->health().error.isEmpty());
}

void TestClaudeUsage::service_config_dir_change_clears_cooldown()
{
    ServiceFixture fx;
    OtherConfigDir other("sub-token");
    primeWithGoodSnapshot(fx);
    fx.service->refresh();
    fx.transport.reply(429, "{}");
    QVERIFY(fx.service->health().retryAtMs > fx.clock);

    fx.service->setConfigDir(other.path());   // same instant, still inside the cooldown

    QCOMPARE(fx.transport.requests.size(), 3);
    QCOMPARE(fx.service->health().failures, 0);
}

void TestClaudeUsage::service_empty_config_dir_without_credentials_is_signed_out()
{
    ServiceFixture fx;
    QTemporaryDir empty;

    fx.service->setConfigDir(empty.path());   // folder exists but holds no credentials

    QCOMPARE(fx.transport.requests.size(), 0);
    QCOMPARE(fx.service->usage().status, Status::Unavailable);
    QVERIFY(!fx.service->buttonVisible());
}

QTEST_GUILESS_MAIN(TestClaudeUsage)
#include "test_claude_usage.moc"
