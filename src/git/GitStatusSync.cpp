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

#include "GitStatusSync.h"

#include "GitStatusModel.h"

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

namespace {
struct Parsed {
    GitStatusEntries entries;
    GitStatusParser::Header header;
};
} // namespace

GitStatusSync::GitStatusSync(GitStatusModel *model, QObject *parent)
    : QObject(parent)
    , m_model(model)
{
}

void GitStatusSync::submit(const QByteArray &porcelainV2)
{
    if (porcelainV2 == m_last) {
        // Same bytes as the parse in flight: it will apply them.
        if (!m_pending) emit unchanged();
        return;
    }
    m_last = porcelainV2;
    const std::uint64_t gen = ++m_generation;
    m_pending = true;

    auto *watcher = new QFutureWatcher<Parsed>(this);
    connect(watcher, &QFutureWatcher<Parsed>::finished, this, [this, watcher, gen]() {
        watcher->deleteLater();
        if (gen != m_generation) return;
        m_pending = false;
        Parsed parsed = watcher->future().takeResult();
        m_model->setEntries(std::move(parsed.entries));
        emit applied(parsed.header);
    });
    watcher->setFuture(QtConcurrent::run([bytes = porcelainV2]() {
        Parsed p;
        p.entries = GitStatusParser::parsePorcelainV2(bytes, &p.header);
        return p;
    }));
}

void GitStatusSync::invalidate()
{
    m_last.clear();
    ++m_generation;
    m_pending = false;
}
