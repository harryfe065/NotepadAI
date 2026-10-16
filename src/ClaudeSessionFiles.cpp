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

#include "ClaudeSessionFiles.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSaveFile>

namespace ClaudeSessionFiles {

namespace {

constexpr qint64 kTailWindowBytes = 64 * 1024;
constexpr qint64 kCopyChunkBytes = 1024 * 1024;
constexpr QByteArrayView kTimestampKey = "\"timestamp\":\"";

QString cleaned(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
}

// Largest `"timestamp":"<iso>"` value on one JSONL line, -1 if none. Only an
// unescaped key matches, so text inside message strings (where quotes are
// escaped) never counts.
qint64 maxTimestampOnLine(QByteArrayView line)
{
    qint64 best = -1;
    qsizetype from = 0;
    while (true) {
        const qsizetype at = line.indexOf(kTimestampKey, from);
        if (at < 0)
            break;
        const qsizetype begin = at + kTimestampKey.size();
        const qsizetype end = line.indexOf('"', begin);
        if (end < 0)
            break;
        const QDateTime dt = QDateTime::fromString(
            QString::fromLatin1(line.sliced(begin, end - begin)), Qt::ISODateWithMs);
        if (dt.isValid())
            best = qMax(best, dt.toMSecsSinceEpoch());
        from = end + 1;
    }
    return best;
}

bool copyFileAtomic(const QString &from, const QString &to, QString *error)
{
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read %1: %2").arg(from, in.errorString());
        return false;
    }
    if (!QDir().mkpath(QFileInfo(to).absolutePath())) {
        *error = QStringLiteral("cannot create %1").arg(QFileInfo(to).absolutePath());
        return false;
    }
    QSaveFile out(to);
    if (!out.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("cannot write %1: %2").arg(to, out.errorString());
        return false;
    }
    QByteArray buf;
    buf.resize(static_cast<qsizetype>(qMin(kCopyChunkBytes, qMax<qint64>(in.size(), 1))));
    while (true) {
        const qint64 n = in.read(buf.data(), buf.size());
        if (n < 0) {
            *error = QStringLiteral("cannot read %1: %2").arg(from, in.errorString());
            out.cancelWriting();
            return false;
        }
        if (n == 0)
            break;
        if (out.write(buf.constData(), n) != n) {
            *error = QStringLiteral("cannot write %1: %2").arg(to, out.errorString());
            out.cancelWriting();
            return false;
        }
    }
    if (!out.commit()) {
        *error = QStringLiteral("cannot write %1: %2").arg(to, out.errorString());
        return false;
    }
    return true;
}

} // namespace

QString configDir(const QMap<QString, QString> &agentEnv,
                  const QProcessEnvironment &processEnv,
                  const QString &homeDir)
{
    static const QString kKey = QStringLiteral("CLAUDE_CONFIG_DIR");
    const QString fromAgent = agentEnv.value(kKey).trimmed();
    if (!fromAgent.isEmpty())
        return cleaned(fromAgent);
    const QString fromProcess = processEnv.value(kKey).trimmed();
    if (!fromProcess.isEmpty())
        return cleaned(fromProcess);
    return cleaned(homeDir) + QStringLiteral("/.claude");
}

bool sameDir(const QString &a, const QString &b)
{
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    return cleaned(a).compare(cleaned(b), cs) == 0;
}

QString encodeProjectDir(const QString &cwd)
{
    QString out = cwd;
    for (QChar &c : out) {
        const ushort u = c.unicode();
        const bool alnum = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
        if (!alnum)
            c = QLatin1Char('-');
    }
    return out;
}

QString locateTranscript(const QString &configDir, const QString &cwd, const QString &sessionId)
{
    if (sessionId.isEmpty())
        return {};
    const QString projects = cleaned(configDir) + QStringLiteral("/projects");
    const QString fileName = sessionId + QStringLiteral(".jsonl");
    const QString own = projects + QLatin1Char('/') + encodeProjectDir(cwd) + QLatin1Char('/') + fileName;
    if (QFileInfo::exists(own))
        return own;
    QDirIterator it(projects, QDir::Dirs | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const QString candidate = it.next() + QLatin1Char('/') + fileName;
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return {};
}

qint64 newestRecordMs(const QString &transcriptPath)
{
    QFile f(transcriptPath);
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    const qint64 size = f.size();
    qint64 window = qMin(kTailWindowBytes, size);
    while (window > 0) {
        if (!f.seek(size - window))
            return -1;
        const QByteArray tail = f.read(window);
        QByteArrayView view(tail);
        // A window that does not reach the start may begin mid-line.
        if (window < size) {
            const qsizetype nl = view.indexOf('\n');
            view = nl < 0 ? QByteArrayView() : view.sliced(nl + 1);
        }
        qsizetype end = view.size();
        while (end > 0) {
            const qsizetype nl = view.first(end).lastIndexOf('\n');
            const qsizetype begin = nl + 1;
            const qint64 ms = maxTimestampOnLine(view.sliced(begin, end - begin));
            if (ms >= 0)
                return ms;
            if (nl < 0)
                break;
            end = nl;
        }
        if (window == size)
            break;
        window = qMin(window * 2, size);
    }
    return -1;
}

CopyOutcome copySession(const QString &transcriptPath, const QString &targetConfigDir)
{
    CopyOutcome outcome;
    const QFileInfo src(transcriptPath);
    const QString folderName = src.absoluteDir().dirName();
    const QString destFolder = cleaned(targetConfigDir) + QStringLiteral("/projects/") + folderName;
    outcome.destPath = destFolder + QLatin1Char('/') + src.fileName();
    if (!copyFileAtomic(src.absoluteFilePath(), outcome.destPath, &outcome.error))
        return outcome;

    const QString sidecar = src.absolutePath() + QLatin1Char('/') + src.completeBaseName();
    if (QFileInfo(sidecar).isDir()) {
        const QDir sidecarDir(sidecar);
        const QString destSidecar = destFolder + QLatin1Char('/') + src.completeBaseName();
        QDirIterator it(sidecar, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            const QString dest = destSidecar + QLatin1Char('/') + sidecarDir.relativeFilePath(file);
            if (!copyFileAtomic(file, dest, &outcome.error))
                return outcome;
        }
    }
    outcome.ok = true;
    return outcome;
}

} // namespace ClaudeSessionFiles
