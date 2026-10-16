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

#ifndef CLAUDE_SESSION_FILES_H
#define CLAUDE_SESSION_FILES_H

#include <QMap>
#include <QString>

class QProcessEnvironment;

// Claude Code's on-disk sessions, for Switch agent (ADR 0003): a session is
// `<config dir>/projects/<encoded cwd>/<id>.jsonl` plus an optional `<id>/`
// sidecar folder (subagent transcripts, tool results). Pure file I/O, no Qt
// GUI; every call is safe on a pool thread.
namespace ClaudeSessionFiles {

// CLAUDE_CONFIG_DIR from the agent's env, else the process env, else
// `<home>/.claude`. Forward slashes, cleaned.
QString configDir(const QMap<QString, QString> &agentEnv,
                  const QProcessEnvironment &processEnv,
                  const QString &homeDir);

// Same directory (case-insensitive on Windows).
bool sameDir(const QString &a, const QString &b);

// Claude Code's project folder name for `cwd`: every non [A-Za-z0-9] -> '-'.
// Long names get a hash suffix the CLI and SDK compute differently, so
// locateTranscript() also scans every project folder.
QString encodeProjectDir(const QString &cwd);

// Absolute path of `<id>.jsonl`: the cwd's folder first, then any folder under
// `projects/`. Empty when absent.
QString locateTranscript(const QString &configDir, const QString &cwd, const QString &sessionId);

// Timestamp (ms since epoch) of the newest record that has one, reading the
// file from its end (64 KiB first, widening only if no timestamp is found).
// -1 when the file is missing or has no timestamped record.
qint64 newestRecordMs(const QString &transcriptPath);

struct CopyOutcome
{
    bool ok = false;
    QString error;
    QString destPath;
};

// Copy the transcript and its sidecar folder into
// `<targetConfigDir>/projects/<same folder name>/`, overwriting older copies.
// Each file is written to a temporary name and renamed, so a reader never sees
// a half-written transcript.
CopyOutcome copySession(const QString &transcriptPath, const QString &targetConfigDir);

} // namespace ClaudeSessionFiles

#endif // CLAUDE_SESSION_FILES_H
