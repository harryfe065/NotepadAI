/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <QtTest>
#include <QJsonArray>
#include <QJsonObject>

#include "AcpScheduledJobs.h"

using AcpProtocol::AcpScheduledJob;
using AcpProtocol::ScheduledJobTracker;

namespace {

constexpr qint64 kNow = 1'000'000'000'000;

// Wire shapes captured from claude-agent-acp 0.89.1.
QJsonObject update(const QString &kind, const QString &toolName, const QString &callId,
                   const QJsonObject &extra, const QJsonObject &toolResponse = {})
{
    QJsonObject claude{{QStringLiteral("toolName"), toolName}};
    if (!toolResponse.isEmpty())
        claude.insert(QStringLiteral("toolResponse"), toolResponse);
    QJsonObject u{{QStringLiteral("sessionUpdate"), kind},
                  {QStringLiteral("toolCallId"), callId},
                  {QStringLiteral("_meta"), QJsonObject{{QStringLiteral("claudeCode"), claude}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        u.insert(it.key(), it.value());
    return u;
}

QJsonObject rawInput(const QJsonObject &o) { return {{QStringLiteral("rawInput"), o}}; }

QJsonObject pending()
{
    return {{QStringLiteral("status"), QStringLiteral("pending")}, {QStringLiteral("rawInput"), QJsonObject{}}};
}

QJsonObject completed(const QString &out)
{
    return {{QStringLiteral("status"), QStringLiteral("completed")},
            {QStringLiteral("rawOutput"), out}};
}

void feedWake(ScheduledJobTracker &t, qint64 now, qint64 scheduledFor, const QString &callId = QStringLiteral("w1"))
{
    const QString tool = QStringLiteral("ScheduleWakeup");
    t.apply(update(QStringLiteral("tool_call"), tool, callId, pending()), now);
    t.apply(update(QStringLiteral("tool_call_update"), tool, callId,
                   rawInput({{QStringLiteral("delaySeconds"), 60},
                             {QStringLiteral("prompt"), QStringLiteral("probe wake fired")},
                             {QStringLiteral("reason"), QStringLiteral("probe")}})), now);
    t.apply(update(QStringLiteral("tool_call_update"), tool, callId, {},
                   {{QStringLiteral("scheduledFor"), scheduledFor},
                    {QStringLiteral("clampedDelaySeconds"), 60}}), now);
    t.apply(update(QStringLiteral("tool_call_update"), tool, callId,
                   completed(QStringLiteral("Next wakeup scheduled"))), now);
}

void feedCron(ScheduledJobTracker &t, qint64 now, const QString &jobId, bool recurring,
              const QString &callId = QStringLiteral("c1"))
{
    const QString tool = QStringLiteral("CronCreate");
    t.apply(update(QStringLiteral("tool_call"), tool, callId, pending()), now);
    t.apply(update(QStringLiteral("tool_call_update"), tool, callId,
                   rawInput({{QStringLiteral("cron"), QStringLiteral("* * * * *")},
                             {QStringLiteral("recurring"), recurring},
                             {QStringLiteral("prompt"), QStringLiteral("probe cron fired")}})), now);
    t.apply(update(QStringLiteral("tool_call_update"), tool, callId, {},
                   {{QStringLiteral("id"), jobId},
                    {QStringLiteral("humanSchedule"), QStringLiteral("Every minute")},
                    {QStringLiteral("recurring"), recurring}}), now);
    t.apply(update(QStringLiteral("tool_call_update"), tool, callId,
                   completed(QStringLiteral("Scheduled job"))), now);
}

// claude-agent-acp 0.90.0: Monitor, then TaskStop on its task.
void feedMonitor(ScheduledJobTracker &t, qint64 now, const QString &taskId, const QJsonObject &toolResponse,
                 const QString &callId = QStringLiteral("m1"))
{
    const QString tool = QStringLiteral("Monitor");
    t.apply(update(QStringLiteral("tool_call"), tool, callId, pending()), now);
    t.apply(update(QStringLiteral("tool_call_update"), tool, callId,
                   rawInput({{QStringLiteral("command"), QStringLiteral("while true; do echo beat; sleep 20; done")},
                             {QStringLiteral("description"), QStringLiteral("heartbeat every 20s")},
                             {QStringLiteral("timeout_ms"), 3600000}})), now);
    if (!toolResponse.isEmpty())
        t.apply(update(QStringLiteral("tool_call_update"), tool, callId, {}, toolResponse), now);
    t.apply(update(QStringLiteral("tool_call_update"), tool, callId,
                   completed(QStringLiteral("Monitor started (task %1, expires in 30m unless the source "
                                            "ends first; ...)").arg(taskId))), now);
}

bool feedTaskStop(ScheduledJobTracker &t, qint64 now, const QString &taskId)
{
    const QString tool = QStringLiteral("TaskStop");
    const QString callId = QStringLiteral("s1");
    t.apply(update(QStringLiteral("tool_call"), tool, callId, pending()), now);
    t.apply(update(QStringLiteral("tool_call_update"), tool, callId,
                   rawInput({{QStringLiteral("task_id"), taskId}})), now);
    t.apply(update(QStringLiteral("tool_call_update"), tool, callId, {},
                   {{QStringLiteral("task_id"), taskId}, {QStringLiteral("task_type"), QStringLiteral("local_bash")}}),
            now);
    return t.apply(update(QStringLiteral("tool_call_update"), tool, callId,
                          completed(QStringLiteral("{\"task_id\":\"%1\"}").arg(taskId))), now);
}

} // namespace

class TestAcpScheduledJobs : public QObject
{
    Q_OBJECT

private slots:
    void wakeup_trackedOnCompletion_withScheduledFor();
    void wakeup_notTrackedBeforeCompletion();
    void wakeup_failedCallIsDropped();
    void wakeup_expiresShortlyAfterFireTime();
    void wakeup_fallsBackToDelaySecondsWithoutToolResponse();
    void wakeup_stopTrueDropsPendingWakeups();
    void cron_recurringTrackedWithJobIdAndSchedule();
    void cron_recurringExpiresAfterSevenDays();
    void cron_oneShotUsesNextFireProvider();
    void cron_deleteRemovesJobById();
    void cron_listReconcilesCronJobs();
    void cron_idParsedFromOutputTextWhenMetaMissing();
    void monitor_trackedUntilItsTimeout();
    void monitor_persistentLivesWithTheSession();
    void monitor_taskIdFromOutputWithoutToolResponse();
    void monitor_taskStopRemovesOnlyThatWatch();
    void unrelatedToolsAreIgnored();
    void clear_dropsEverythingAndReportsChange();
    void expire_falseWhenNothingExpired();
    void nextExpiry_isEarliestAcrossJobs();
};

void TestAcpScheduledJobs::wakeup_trackedOnCompletion_withScheduledFor()
{
    ScheduledJobTracker t;
    feedWake(t, kNow, kNow + 90'000);
    QCOMPARE(t.jobs().size(), 1);
    const AcpScheduledJob &j = t.jobs().first();
    QCOMPARE(j.kind, AcpScheduledJob::Kind::Wake);
    QCOMPARE(j.fireAtMs, kNow + 90'000);
    QCOMPARE(j.prompt, QStringLiteral("probe wake fired"));
}

void TestAcpScheduledJobs::wakeup_notTrackedBeforeCompletion()
{
    ScheduledJobTracker t;
    const QString tool = QStringLiteral("ScheduleWakeup");
    t.apply(update(QStringLiteral("tool_call"), tool, QStringLiteral("w1"), pending()), kNow);
    t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("w1"),
                   rawInput({{QStringLiteral("delaySeconds"), 60}})), kNow);
    QVERIFY(t.jobs().isEmpty());
}

void TestAcpScheduledJobs::wakeup_failedCallIsDropped()
{
    ScheduledJobTracker t;
    const QString tool = QStringLiteral("ScheduleWakeup");
    t.apply(update(QStringLiteral("tool_call"), tool, QStringLiteral("w1"), pending()), kNow);
    QVERIFY(!t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("w1"),
                            {{QStringLiteral("status"), QStringLiteral("failed")}}), kNow));
    QVERIFY(t.jobs().isEmpty());
    // A late completed for the same id must not resurrect a job with no data.
    t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("w1"),
                   completed(QStringLiteral("x"))), kNow);
    QVERIFY(t.jobs().isEmpty());
}

void TestAcpScheduledJobs::wakeup_expiresShortlyAfterFireTime()
{
    ScheduledJobTracker t;
    feedWake(t, kNow, kNow + 90'000);
    // Observed: the wake turn starts ~5 s after the scheduled time.
    QVERIFY(!t.expire(kNow + 90'000 + 5'000));
    QCOMPARE(t.jobs().size(), 1);
    QVERIFY(t.expire(kNow + 90'000 + ScheduledJobTracker::kWakeGraceMs));
    QVERIFY(t.jobs().isEmpty());
}

void TestAcpScheduledJobs::wakeup_fallsBackToDelaySecondsWithoutToolResponse()
{
    ScheduledJobTracker t;
    const QString tool = QStringLiteral("ScheduleWakeup");
    t.apply(update(QStringLiteral("tool_call"), tool, QStringLiteral("w1"), pending()), kNow);
    t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("w1"),
                   rawInput({{QStringLiteral("delaySeconds"), 120}})), kNow);
    t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("w1"),
                   completed(QStringLiteral("ok"))), kNow);
    QCOMPARE(t.jobs().size(), 1);
    QCOMPARE(t.jobs().first().fireAtMs, kNow + 120'000);
}

void TestAcpScheduledJobs::wakeup_stopTrueDropsPendingWakeups()
{
    ScheduledJobTracker t;
    feedWake(t, kNow, kNow + 90'000);
    feedCron(t, kNow, QStringLiteral("aa"), true);
    const QString tool = QStringLiteral("ScheduleWakeup");
    t.apply(update(QStringLiteral("tool_call"), tool, QStringLiteral("w2"), pending()), kNow);
    t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("w2"),
                   rawInput({{QStringLiteral("stop"), true}})), kNow);
    QVERIFY(t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("w2"),
                           completed(QStringLiteral("Wake loop ended"))), kNow));
    // Only the wake-up goes; the cron job is a separate scheduler.
    QCOMPARE(t.jobs().size(), 1);
    QCOMPARE(t.jobs().first().kind, AcpScheduledJob::Kind::Cron);
}

void TestAcpScheduledJobs::cron_recurringTrackedWithJobIdAndSchedule()
{
    ScheduledJobTracker t;
    feedCron(t, kNow, QStringLiteral("3d4e0778"), true);
    QCOMPARE(t.jobs().size(), 1);
    const AcpScheduledJob &j = t.jobs().first();
    QCOMPARE(j.kind, AcpScheduledJob::Kind::Cron);
    QCOMPARE(j.id, QStringLiteral("3d4e0778"));
    QCOMPARE(j.cron, QStringLiteral("* * * * *"));
    QCOMPARE(j.humanSchedule, QStringLiteral("Every minute"));
    QVERIFY(j.recurring);
}

void TestAcpScheduledJobs::cron_recurringExpiresAfterSevenDays()
{
    ScheduledJobTracker t;
    feedCron(t, kNow, QStringLiteral("aa"), true);
    const qint64 week = 7LL * 24 * 3600 * 1000;
    QVERIFY(!t.expire(kNow + week - 1));
    QVERIFY(t.expire(kNow + week));
    QVERIFY(t.jobs().isEmpty());
}

void TestAcpScheduledJobs::cron_oneShotUsesNextFireProvider()
{
    ScheduledJobTracker t;
    QString seenCron;
    t.setNextFireProvider([&](const QString &cron, qint64 from) {
        seenCron = cron;
        return from + 600'000;
    });
    feedCron(t, kNow, QStringLiteral("bb"), false);
    QCOMPARE(seenCron, QStringLiteral("* * * * *"));
    QCOMPARE(t.jobs().size(), 1);
    QVERIFY(!t.jobs().first().recurring);
    QVERIFY(!t.expire(kNow + 600'000));
    QVERIFY(t.expire(kNow + 600'000 + ScheduledJobTracker::kWakeGraceMs));
}

void TestAcpScheduledJobs::cron_deleteRemovesJobById()
{
    ScheduledJobTracker t;
    feedCron(t, kNow, QStringLiteral("aa"), true, QStringLiteral("c1"));
    feedCron(t, kNow, QStringLiteral("bb"), true, QStringLiteral("c2"));
    QCOMPARE(t.jobs().size(), 2);

    const QString tool = QStringLiteral("CronDelete");
    t.apply(update(QStringLiteral("tool_call"), tool, QStringLiteral("d1"), pending()), kNow);
    t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("d1"),
                   rawInput({{QStringLiteral("id"), QStringLiteral("aa")}})), kNow);
    QVERIFY(t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("d1"),
                           completed(QStringLiteral("Cancelled job aa."))), kNow));
    QCOMPARE(t.jobs().size(), 1);
    QCOMPARE(t.jobs().first().id, QStringLiteral("bb"));
}

void TestAcpScheduledJobs::cron_listReconcilesCronJobs()
{
    ScheduledJobTracker t;
    feedCron(t, kNow, QStringLiteral("aa"), false, QStringLiteral("c1"));
    feedWake(t, kNow, kNow + 90'000);
    // A one-shot fired and auto-deleted; the authoritative list is now empty.
    const QString tool = QStringLiteral("CronList");
    t.apply(update(QStringLiteral("tool_call"), tool, QStringLiteral("l1"), pending()), kNow);
    t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("l1"), {},
                   {{QStringLiteral("jobs"), QJsonArray{}}}), kNow);
    t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("l1"),
                   completed(QStringLiteral("No scheduled jobs."))), kNow);
    // Cron jobs reconciled away; the wake-up is not part of CronList and stays.
    QCOMPARE(t.jobs().size(), 1);
    QCOMPARE(t.jobs().first().kind, AcpScheduledJob::Kind::Wake);
}

void TestAcpScheduledJobs::cron_idParsedFromOutputTextWhenMetaMissing()
{
    ScheduledJobTracker t;
    const QString tool = QStringLiteral("CronCreate");
    t.apply(update(QStringLiteral("tool_call"), tool, QStringLiteral("c1"), pending()), kNow);
    t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("c1"),
                   rawInput({{QStringLiteral("cron"), QStringLiteral("0 9 * * *")}})), kNow);
    t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("c1"),
                   completed(QStringLiteral("Scheduled recurring job 134dd71d (Every minute). Session-only"))),
            kNow);
    QCOMPARE(t.jobs().size(), 1);
    QCOMPARE(t.jobs().first().id, QStringLiteral("134dd71d"));
    QVERIFY(t.jobs().first().recurring);
}

void TestAcpScheduledJobs::monitor_trackedUntilItsTimeout()
{
    ScheduledJobTracker t;
    feedMonitor(t, kNow, QStringLiteral("b9727pa2e"),
                {{QStringLiteral("taskId"), QStringLiteral("b9727pa2e")},
                 {QStringLiteral("timeoutMs"), 1800000}, // granted, not the 3600000 asked for
                 {QStringLiteral("persistent"), false}});
    QCOMPARE(t.jobs().size(), 1);
    const AcpScheduledJob &j = t.jobs().first();
    QCOMPARE(j.kind, AcpScheduledJob::Kind::Monitor);
    QCOMPARE(j.id, QStringLiteral("b9727pa2e"));
    QCOMPARE(j.prompt, QStringLiteral("heartbeat every 20s"));
    QCOMPARE(j.command, QStringLiteral("while true; do echo beat; sleep 20; done"));
    QCOMPARE(j.fireAtMs, kNow);
    QCOMPARE(j.expiresAtMs, kNow + 1800000);
    QVERIFY(AcpProtocol::formatScheduledJob(j).contains(QStringLiteral("heartbeat every 20s")));
    QVERIFY(!t.expire(kNow + 1799999));
    QVERIFY(t.expire(kNow + 1800000));
}

void TestAcpScheduledJobs::monitor_persistentLivesWithTheSession()
{
    ScheduledJobTracker t;
    feedMonitor(t, kNow, QStringLiteral("p1"),
                {{QStringLiteral("taskId"), QStringLiteral("p1")}, {QStringLiteral("persistent"), true}});
    QCOMPARE(t.jobs().size(), 1);
    QCOMPARE(t.jobs().first().expiresAtMs, kNow + ScheduledJobTracker::kRecurringLifetimeMs);
}

void TestAcpScheduledJobs::monitor_taskIdFromOutputWithoutToolResponse()
{
    ScheduledJobTracker t;
    feedMonitor(t, kNow, QStringLiteral("b6x8d7os3"), {});
    QCOMPARE(t.jobs().size(), 1);
    QCOMPARE(t.jobs().first().id, QStringLiteral("b6x8d7os3"));
    QCOMPARE(t.jobs().first().expiresAtMs, kNow + 3600000); // rawInput timeout_ms
}

void TestAcpScheduledJobs::monitor_taskStopRemovesOnlyThatWatch()
{
    ScheduledJobTracker t;
    feedMonitor(t, kNow, QStringLiteral("a1"), {{QStringLiteral("taskId"), QStringLiteral("a1")}},
                QStringLiteral("m1"));
    feedMonitor(t, kNow, QStringLiteral("a2"), {{QStringLiteral("taskId"), QStringLiteral("a2")}},
                QStringLiteral("m2"));
    feedCron(t, kNow, QStringLiteral("a1"), true); // same id, other kind: untouched
    QCOMPARE(t.jobs().size(), 3);
    QVERIFY(!feedTaskStop(t, kNow, QStringLiteral("zz"))); // a Bash task, not a watch
    QVERIFY(feedTaskStop(t, kNow, QStringLiteral("a1")));
    QCOMPARE(t.jobs().size(), 2);
    for (const AcpScheduledJob &j : t.jobs())
        QVERIFY(!(j.kind == AcpScheduledJob::Kind::Monitor && j.id == QStringLiteral("a1")));
}

void TestAcpScheduledJobs::unrelatedToolsAreIgnored()
{
    ScheduledJobTracker t;
    const QString tool = QStringLiteral("Bash");
    QVERIFY(!t.apply(update(QStringLiteral("tool_call"), tool, QStringLiteral("b1"), pending()), kNow));
    QVERIFY(!t.apply(update(QStringLiteral("tool_call_update"), tool, QStringLiteral("b1"),
                            completed(QStringLiteral("done"))), kNow));
    QVERIFY(t.jobs().isEmpty());
    QVERIFY(!t.apply(QJsonObject{}, kNow));
}

void TestAcpScheduledJobs::clear_dropsEverythingAndReportsChange()
{
    ScheduledJobTracker t;
    QVERIFY(!t.clear());
    feedCron(t, kNow, QStringLiteral("aa"), true);
    QVERIFY(t.clear());
    QVERIFY(t.jobs().isEmpty());
}

void TestAcpScheduledJobs::expire_falseWhenNothingExpired()
{
    ScheduledJobTracker t;
    feedCron(t, kNow, QStringLiteral("aa"), true);
    QVERIFY(!t.expire(kNow + 1000));
    QCOMPARE(t.jobs().size(), 1);
}

void TestAcpScheduledJobs::nextExpiry_isEarliestAcrossJobs()
{
    ScheduledJobTracker t;
    QCOMPARE(t.nextExpiryMs(), qint64(0));
    feedCron(t, kNow, QStringLiteral("aa"), true);
    feedWake(t, kNow, kNow + 90'000);
    QCOMPARE(t.nextExpiryMs(), kNow + 90'000 + ScheduledJobTracker::kWakeGraceMs);
}

QTEST_GUILESS_MAIN(TestAcpScheduledJobs)
#include "test_acp_scheduled_jobs.moc"
