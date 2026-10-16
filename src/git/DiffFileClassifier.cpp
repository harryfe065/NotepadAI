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

#include "DiffFileClassifier.h"

#include <QByteArrayView>

#include <array>

namespace DiffFileClassifier {

namespace {

QByteArrayView baseName(QByteArrayView path)
{
    const qsizetype slash = path.lastIndexOf('/');
    return slash < 0 ? path : path.sliced(slash + 1);
}

bool isLockfile(QByteArrayView path)
{
    static constexpr std::array<QByteArrayView, 11> kNames{
        "package-lock.json", "pnpm-lock.yaml", "bun.lock", "bun.lockb",
        "poetry.lock", "Pipfile.lock", "Gemfile.lock", "composer.lock",
        "go.sum", "flake.lock", "packages.lock.json",
    };
    const QByteArrayView name = baseName(path);
    if (name.endsWith(".lock")) return true;  // yarn.lock, Cargo.lock, ...
    for (QByteArrayView n : kNames)
        if (name == n) return true;
    return false;
}

bool looksGenerated(QByteArrayView path)
{
    static constexpr std::array<QByteArrayView, 8> kSuffixes{
        ".min.js", ".min.css", ".map", "_pb2.py", ".pb.go", ".pb.cc", ".pb.h", ".g.dart",
    };
    const QByteArrayView name = baseName(path);
    for (QByteArrayView s : kSuffixes)
        if (name.endsWith(s)) return true;
    if (name.contains(".generated.")) return true;
    return path.startsWith("dist/") || path.contains("/dist/");
}

bool isLarge(const GitDiffParser::FileSpan &f)
{
    return f.bodyRows > kMaxRows || f.bodyBytes > kMaxBytes || f.maxLineBytes > kMaxLineBytes;
}

} // namespace

Reason classify(const GitDiffParser::FileSpan &f, bool linguistGenerated)
{
    if (f.binary) return Reason::Binary;
    if (f.submodule) return Reason::Submodule;
    if (f.bodyRows == 0) {
        if (f.status == 'R' || f.status == 'C') return Reason::RenamedOnly;
        if (f.oldMode && f.newMode && f.oldMode != f.newMode) return Reason::ModeOnly;
    }
    const QByteArrayView path = f.path();
    if (isLockfile(path)) return Reason::Lockfile;
    if (linguistGenerated || looksGenerated(path)) return Reason::Generated;
    if (f.status == 'D') return Reason::Deleted;
    if (isLarge(f)) return Reason::Large;
    return Reason::None;
}

bool startsCollapsed(Reason reason, qsizetype fileCount)
{
    if (fileCount < 2) return false;
    switch (reason) {
        case Reason::Binary:
        case Reason::Lockfile:
        case Reason::Generated:
        case Reason::Deleted:
        case Reason::Large:
            return true;
        case Reason::None:
        case Reason::Submodule:
        case Reason::RenamedOnly:
        case Reason::ModeOnly:
            break;
    }
    return false;
}

bool canLoad(Reason reason)
{
    switch (reason) {
        case Reason::Lockfile:
        case Reason::Generated:
        case Reason::Deleted:
        case Reason::Large:
            return true;
        case Reason::None:
        case Reason::Binary:
        case Reason::Submodule:
        case Reason::RenamedOnly:
        case Reason::ModeOnly:
            break;
    }
    return false;
}

} // namespace DiffFileClassifier
