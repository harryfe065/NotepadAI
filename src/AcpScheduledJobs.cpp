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

#include "AcpScheduledJobs.h"

#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace AcpProtocol {

namespace {

constexpr int kTooltipPromptMaxChars = 80;

bool isScheduleTool(const QString &tool)
{
    return tool == QLatin1String("ScheduleWakeup") || tool == QLatin1String("CronCreate")
        || tool == QLatin1String("CronDelete") || tool == QLatin1String("CronList");
}

QString toolNameOf(const QJsonObject &update)
{
    const QString meta = update.value(QStringLiteral("_meta")).toObject()
        .value(QStringLiteral("claudeCode")).toObject()
        .value(QStringLiteral("toolName")).toString();
    return meta.isEmpty() ? update.value(QStringLiteral("name")).toString() : meta;
}

// Final text of a tool result: `rawOutput` is a plain string for these tools;
// fall back to the first text content block.
QString outputTextOf(const QJsonObject &update)
{
    const QJsonValue raw = update.value(QStringLiteral("rawOutput"));
    if (raw.isString())
        return raw.toString();
    const QJsonArray content = update.value(QStringLiteral("content")).toArray();
    if (!content.isEmpty())
        return content.first().toObject().value(QStringLiteral("content")).toObject()
            .value(QStringLiteral("text")).toString();
    return {};
}

void mergeInto(QJsonObject &dst, const QJsonObject &src)
{
    for (auto it = src.begin(); it != src.end(); ++it)
        dst.insert(it.key(), it.value());
}

} // namespace

bool ScheduledJobTracker::apply(const QJsonObject &update, qint64 nowMs)
{
    const QString kind = update.value(QStringLiteral("sessionUpdate")).toString();
    if (kind != QLatin1String("tool_call") && kind != QLatin1String("tool_call_update"))
        return false;
    const QString tool = toolNameOf(update);
    const QString id = update.value(QStringLiteral("toolCallId")).toString();
    if (id.isEmpty() || !isScheduleTool(tool))
        return false;

    PendingCall &call = m_pending[id];
    call.tool = tool;
    const QJsonValue input = update.value(QStringLiteral("rawInput"));
    if (input.isObject())
        mergeInto(call.input, input.toObject());
    const QJsonValue response = update.value(QStringLiteral("_meta")).toObject()
        .value(QStringLiteral("claudeCode")).toObject().value(QStringLiteral("toolResponse"));
    if (response.isObject())
        mergeInto(call.response, response.toObject());
    const QString text = outputTextOf(update);
    if (!text.isEmpty())
        call.output = text;

    const QString status = update.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("failed")) {
        m_pending.remove(id);
        return false;
    }
    if (status != QLatin1String("completed"))
        return false;
    const PendingCall done = m_pending.take(id);
    return commit(id, done, nowMs);
}

bool ScheduledJobTracker::commit(const QString &toolCallId, const PendingCall &call, qint64 nowMs)
{
    const QString prompt = call.input.value(QStringLiteral("prompt")).toString();

    if (call.tool == QLatin1String("ScheduleWakeup")) {
        // `stop:true` ends the wake loop: no further wake-ups fire.
        if (call.input.value(QStringLiteral("stop")).toBool(false)) {
            const qsizetype before = m_jobs.size();
            m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                        [](const AcpScheduledJob &j) { return j.kind == AcpScheduledJob::Kind::Wake; }),
                         m_jobs.end());
            return m_jobs.size() != before;
        }
        qint64 fireAt = static_cast<qint64>(call.response.value(QStringLiteral("scheduledFor")).toDouble(0));
        if (fireAt <= 0) {
            const double delay = call.input.value(QStringLiteral("delaySeconds")).toDouble(0);
            if (delay <= 0)
                return false;
            fireAt = nowMs + static_cast<qint64>(delay * 1000);
        }
        AcpScheduledJob job;
        job.kind = AcpScheduledJob::Kind::Wake;
        job.id = toolCallId;
        job.prompt = prompt;
        job.fireAtMs = fireAt;
        job.expiresAtMs = fireAt + kWakeGraceMs;
        m_jobs.append(job);
        return true;
    }

    if (call.tool == QLatin1String("CronCreate")) {
        QString jobId = call.response.value(QStringLiteral("id")).toString();
        if (jobId.isEmpty()) {
            static const QRegularExpression idRe(QStringLiteral("(?:job|task) ([0-9a-f]{6,})"));
            jobId = idRe.match(call.output).captured(1);
        }
        const QString cron = call.input.value(QStringLiteral("cron")).toString();
        if (jobId.isEmpty() || cron.isEmpty())
            return false;
        const QJsonValue recurringMeta = call.response.value(QStringLiteral("recurring"));
        const bool recurring = recurringMeta.isBool()
            ? recurringMeta.toBool()
            : call.input.value(QStringLiteral("recurring")).toBool(true);

        AcpScheduledJob job;
        job.kind = AcpScheduledJob::Kind::Cron;
        job.id = jobId;
        job.prompt = prompt;
        job.cron = cron;
        job.humanSchedule = call.response.value(QStringLiteral("humanSchedule")).toString();
        job.recurring = recurring;
        job.expiresAtMs = nowMs + kRecurringLifetimeMs;
        if (!recurring && m_nextFire) {
            const qint64 next = m_nextFire(cron, nowMs);
            if (next > 0)
                job.expiresAtMs = next + kWakeGraceMs;
        }
        removeCron(jobId); // same id re-created: keep one entry
        m_jobs.append(job);
        return true;
    }

    if (call.tool == QLatin1String("CronDelete")) {
        QString jobId = call.input.value(QStringLiteral("id")).toString();
        if (jobId.isEmpty())
            jobId = call.response.value(QStringLiteral("id")).toString();
        return !jobId.isEmpty() && removeCron(jobId);
    }

    if (call.tool == QLatin1String("CronList")) {
        const QJsonValue jobs = call.response.value(QStringLiteral("jobs"));
        if (!jobs.isArray())
            return false;
        QSet<QString> live;
        for (const QJsonValue &j : jobs.toArray())
            live.insert(j.toObject().value(QStringLiteral("id")).toString());
        const qsizetype before = m_jobs.size();
        m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                    [&live](const AcpScheduledJob &j) {
                                        return j.kind == AcpScheduledJob::Kind::Cron && !live.contains(j.id);
                                    }),
                     m_jobs.end());
        return m_jobs.size() != before;
    }
    return false;
}

bool ScheduledJobTracker::removeCron(const QString &id)
{
    const qsizetype before = m_jobs.size();
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                [&id](const AcpScheduledJob &j) {
                                    return j.kind == AcpScheduledJob::Kind::Cron && j.id == id;
                                }),
                 m_jobs.end());
    return m_jobs.size() != before;
}

bool ScheduledJobTracker::expire(qint64 nowMs)
{
    const qsizetype before = m_jobs.size();
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                [nowMs](const AcpScheduledJob &j) { return j.expiresAtMs <= nowMs; }),
                 m_jobs.end());
    return m_jobs.size() != before;
}

bool ScheduledJobTracker::clear()
{
    m_pending.clear();
    if (m_jobs.isEmpty())
        return false;
    m_jobs.clear();
    return true;
}

qint64 ScheduledJobTracker::nextExpiryMs() const
{
    qint64 next = 0;
    for (const AcpScheduledJob &j : m_jobs) {
        if (next == 0 || j.expiresAtMs < next)
            next = j.expiresAtMs;
    }
    return next;
}

QString formatScheduledJob(const AcpScheduledJob &job)
{
    QString head;
    if (job.kind == AcpScheduledJob::Kind::Wake) {
        head = QStringLiteral("Wake-up %1")
                   .arg(QDateTime::fromMSecsSinceEpoch(job.fireAtMs).toString(QStringLiteral("HH:mm:ss")));
    } else {
        head = QStringLiteral("%1 (%2)")
                   .arg(job.humanSchedule.isEmpty() ? job.cron : job.humanSchedule,
                        job.recurring ? QStringLiteral("recurring") : QStringLiteral("once"));
    }
    QString prompt = job.prompt.simplified();
    if (prompt.isEmpty())
        return head;
    if (prompt.size() > kTooltipPromptMaxChars)
        prompt = prompt.left(kTooltipPromptMaxChars) + QChar(0x2026);
    return head + QStringLiteral(" — ") + prompt;
}

} // namespace AcpProtocol
