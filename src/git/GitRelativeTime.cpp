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

#include "GitRelativeTime.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>

namespace GitRelativeTime {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("GitRelativeTime", text);
}

QString count(qint64 n, const char *one, const char *many)
{
    return n == 1 ? tr(one) : tr(many).arg(n);
}

} // namespace

QString format(qint64 secs, qint64 now)
{
    if (secs <= 0) return {};
    const qint64 d = qMax<qint64>(0, now - secs);
    if (d < 60) return tr("just now");
    if (d < 3600) return count(d / 60, "1 minute ago", "%1 minutes ago");
    if (d < 86400) return count(d / 3600, "1 hour ago", "%1 hours ago");
    if (d < 7 * 86400) return count(d / 86400, "1 day ago", "%1 days ago");
    if (d < 30 * 86400) return count(d / (7 * 86400), "1 week ago", "%1 weeks ago");
    return QLocale::system().toString(QDateTime::fromSecsSinceEpoch(secs).date(),
                                      QLocale::ShortFormat);
}

QString localTime(qint64 secs)
{
    return QDateTime::fromSecsSinceEpoch(secs).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace GitRelativeTime
