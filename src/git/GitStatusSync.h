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

#ifndef GIT_STATUS_SYNC_H
#define GIT_STATUS_SYNC_H

#include "GitStatusParser.h"

#include <QByteArray>
#include <QObject>

#include <cstdint>

class GitStatusModel;

// Turns raw `git status --porcelain=v2 --branch -z` output into model updates
// without parsing on the UI thread.
//
// Output identical to the last submitted one is dropped on the spot (a memcmp):
// an agent editing files that are already dirty leaves status unchanged, so
// those refreshes cost nothing here. Different output is parsed on the global
// thread pool and applied with GitStatusModel::setEntries (in place, no reset).
// Latest submit wins: a parse that finishes after a newer submit is dropped.
class GitStatusSync : public QObject
{
    Q_OBJECT
public:
    explicit GitStatusSync(GitStatusModel *model, QObject *parent = nullptr);

    void submit(const QByteArray &porcelainV2);
    // Forget the last output and drop any parse in flight (repo switch).
    void invalidate();
    // A parse is running; applied() or nothing (if superseded) follows.
    bool pending() const { return m_pending; }

signals:
    // The model now reflects the submitted output.
    void applied(const GitStatusParser::Header &header);
    // The submitted output equals the last one; the model is untouched.
    void unchanged();

private:
    GitStatusModel *m_model;
    QByteArray m_last;
    std::uint64_t m_generation = 0;
    bool m_pending = false;
};

#endif // GIT_STATUS_SYNC_H
