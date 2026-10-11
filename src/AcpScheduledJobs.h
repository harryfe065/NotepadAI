/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef ACP_SCHEDULED_JOBS_H
#define ACP_SCHEDULED_JOBS_H

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>

namespace AcpProtocol {

// A wake-up or cron job Claude Code scheduled for itself (ScheduleWakeup /
// CronCreate). The agent re-invokes the model with `prompt` when it fires, as
// an unsolicited turn with no session/prompt. Display-only: neither
// `session/cancel` nor `_session/async_task/stop` can cancel these.
struct AcpScheduledJob
{
    enum class Kind { Wake, Cron };

    Kind kind = Kind::Wake;
    QString id;            // cron job id; the tool-call id for a wake-up
    QString prompt;
    QString cron;          // Cron only
    QString humanSchedule; // Cron only, e.g. "Every minute"
    bool recurring = false;
    qint64 fireAtMs = 0;   // Wake only: epoch ms the agent will wake
    qint64 expiresAtMs = 0; // when the host drops the job from its live list
};

// Live list of scheduled jobs, rebuilt from the tool-call stream. Pure: the
// caller supplies the clock, so tests need no timers. Wire shapes
// (claude-agent-acp 0.89.1): `_meta.claudeCode.toolName`, `rawInput` arriving
// in pieces, `_meta.claudeCode.toolResponse` carrying the job id / schedule,
// and a final `status:"completed"` update.
class ScheduledJobTracker
{
public:
    // A wake turn starts ~5 s after its scheduled time; keep the job listed
    // through it.
    static constexpr qint64 kWakeGraceMs = 30'000;
    // Claude Code auto-expires recurring jobs after 7 days.
    static constexpr qint64 kRecurringLifetimeMs = 7LL * 24 * 3600 * 1000;

    // Next fire time (epoch ms) of a cron expression strictly after `fromMs`;
    // <= 0 when unknown. Used to expire one-shot cron jobs.
    using NextFireFn = std::function<qint64(const QString &cron, qint64 fromMs)>;
    void setNextFireProvider(NextFireFn fn) { m_nextFire = std::move(fn); }

    // Feed one `session/update` payload (the `update` object). Returns true
    // when the live job list changed.
    bool apply(const QJsonObject &update, qint64 nowMs);
    // Drop jobs whose expiry has passed. Returns true when any were dropped.
    bool expire(qint64 nowMs);
    // Drop everything (agent process gone: session-only jobs die with it).
    bool clear();

    const QVector<AcpScheduledJob> &jobs() const { return m_jobs; }
    // Earliest `expiresAtMs`, or 0 when no job is live.
    qint64 nextExpiryMs() const;

private:
    struct PendingCall
    {
        QString tool;
        QJsonObject input;
        QJsonObject response;
        QString output;
    };

    bool commit(const QString &toolCallId, const PendingCall &call, qint64 nowMs);
    bool removeCron(const QString &id);

    QHash<QString, PendingCall> m_pending; // tool calls not yet completed
    QVector<AcpScheduledJob> m_jobs;       // creation order
    NextFireFn m_nextFire;
};

// One-line tooltip entry: "Wake-up 14:12:00 — <prompt>" / "Every minute (recurring) — <prompt>".
QString formatScheduledJob(const AcpScheduledJob &job);

} // namespace AcpProtocol

#endif // ACP_SCHEDULED_JOBS_H
