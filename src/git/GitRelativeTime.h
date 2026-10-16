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

#ifndef GIT_RELATIVE_TIME_H
#define GIT_RELATIVE_TIME_H

#include <QString>

namespace GitRelativeTime {

// "just now", "5 minutes ago", ... "3 weeks ago"; past ~30 days the date in
// the system locale's short format. Empty for `secs <= 0`. Pure, so it can
// run on the pool and in tests with a fixed `now`.
QString format(qint64 secs, qint64 now);

// Local time "yyyy-MM-dd HH:mm" for tooltips.
QString localTime(qint64 secs);

} // namespace GitRelativeTime

#endif // GIT_RELATIVE_TIME_H
