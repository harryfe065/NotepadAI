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

#include "PathStatusIndex.h"

#include <QDir>

#include <array>
#include <cstdint>

namespace {

// Priority table — higher value = "worse" status; rollup picks the worst.
// Order is part of the file-tree-git-decorations capability contract and
// MUST stay in sync with specs/file-tree-git-decorations/spec.md's
// "Requirement: Folder rows roll up to the worst-status descendant colour".
constexpr std::array<std::uint8_t, GitStatusEntry::Unmerged + 1> kPriority = []() {
    std::array<std::uint8_t, GitStatusEntry::Unmerged + 1> a{};
    a[GitStatusEntry::Untracked_]  = 10;
    a[GitStatusEntry::TypeChanged] = 15;
    a[GitStatusEntry::Copied]      = 20;
    a[GitStatusEntry::Renamed]     = 30;
    a[GitStatusEntry::Added]       = 40;
    a[GitStatusEntry::Modified]    = 50;
    a[GitStatusEntry::Deleted]     = 60;
    a[GitStatusEntry::Unmerged]    = 70;
    return a;
}();

} // namespace

GitStatusEntry::Change PathStatusIndex::worseChange(GitStatusEntry::Change a,
                                                    GitStatusEntry::Change b)
{
    return kPriority[a] >= kPriority[b] ? a : b;
}

void PathStatusIndex::clear()
{
    m_files.clear();
    m_folders.clear();
}

void PathStatusIndex::rebuild(const GitStatusEntries &entries,
                              const QString &repoToplevel)
{
    m_files.clear();
    m_folders.clear();

    if (entries.isEmpty() || repoToplevel.isEmpty()) {
        return;
    }

    const QString root = QDir::cleanPath(repoToplevel);
    // Pre-size for the typical case (modest dirty set, fanout ~average_depth).
    m_files.reserve(entries.size());
    m_folders.reserve(entries.size() * 4);

    const QChar slash = QLatin1Char('/');

    for (const GitStatusEntry &e : entries) {
        if (e.relPath.isEmpty()) continue;

        const QString abs = QDir::cleanPath(root + slash + e.relPath);

        if (e.isSubmodule) {
            // Submodule path is a directory on disk — register as a folder so
            // the row paints. Per-file colours INSIDE the submodule come from
            // a separate `git status` invocation in that submodule's working
            // tree (entries are merged into the same rebuild call), not from
            // propagating the parent's aggregate change downward.
            auto fit = m_folders.find(abs);
            if (fit == m_folders.end()) {
                m_folders.insert(abs, e.change);
            } else {
                *fit = worseChange(*fit, e.change);
            }
        } else {
            auto it = m_files.find(abs);
            if (it == m_files.end()) {
                m_files.insert(abs, e.change);
            } else {
                *it = worseChange(*it, e.change);
            }
        }

        // Folder rollup: walk parents up to (but excluding) root. Compare
        // by string prefix because QFileInfo::absolutePath would allocate
        // repeatedly inside the loop. Every folder is at least as bad as its
        // children, so once an ancestor already holds a status at least this
        // bad, every folder above it does too: stop there. Siblings then cost
        // one lookup instead of one per directory level.
        int cut = abs.lastIndexOf(slash);
        while (cut > root.size()) {
            const QString ancestor = abs.left(cut);
            // `ancestor.size() > root.size()` plus the slash check above
            // guarantees `ancestor != root`.
            auto fit = m_folders.find(ancestor);
            if (fit == m_folders.end()) {
                m_folders.insert(ancestor, e.change);
            } else if (worseChange(*fit, e.change) == *fit) {
                break;
            } else {
                *fit = e.change;
            }
            cut = abs.lastIndexOf(slash, cut - 1);
        }
    }
}

std::optional<GitStatusEntry::Change>
PathStatusIndex::fileChange(const QString &path) const
{
    if (m_files.isEmpty()) return std::nullopt;
    const QString clean = QDir::cleanPath(path);
    auto it = m_files.constFind(clean);
    if (it == m_files.constEnd()) return std::nullopt;
    return *it;
}

std::optional<GitStatusEntry::Change>
PathStatusIndex::folderChange(const QString &path) const
{
    if (m_folders.isEmpty()) return std::nullopt;
    const QString clean = QDir::cleanPath(path);
    auto it = m_folders.constFind(clean);
    if (it == m_folders.constEnd()) return std::nullopt;
    return *it;
}

QSet<QString> PathStatusIndex::deltaPaths(const PathStatusIndex &previous) const
{
    QSet<QString> out;
    out.reserve(m_files.size() + previous.m_files.size()
                + m_folders.size() + previous.m_folders.size());

    // Forward pass: entries present (or differing) here vs. previous.
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        const auto pit = previous.m_files.constFind(it.key());
        if (pit == previous.m_files.constEnd() || *pit != *it) {
            out.insert(it.key());
        }
    }
    for (auto it = m_folders.constBegin(); it != m_folders.constEnd(); ++it) {
        const auto pit = previous.m_folders.constFind(it.key());
        if (pit == previous.m_folders.constEnd() || *pit != *it) {
            out.insert(it.key());
        }
    }

    // Reverse pass: entries that were in previous but no longer in this.
    // (Differing values are already captured above; only "removed" remain.)
    for (auto it = previous.m_files.constBegin();
         it != previous.m_files.constEnd(); ++it) {
        if (!m_files.contains(it.key())) {
            out.insert(it.key());
        }
    }
    for (auto it = previous.m_folders.constBegin();
         it != previous.m_folders.constEnd(); ++it) {
        if (!m_folders.contains(it.key())) {
            out.insert(it.key());
        }
    }

    return out;
}

QSet<QString> PathStatusIndex::allIndexedPaths() const
{
    QSet<QString> out;
    out.reserve(m_files.size() + m_folders.size());
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        out.insert(it.key());
    }
    for (auto it = m_folders.constBegin(); it != m_folders.constEnd(); ++it) {
        out.insert(it.key());
    }
    return out;
}
