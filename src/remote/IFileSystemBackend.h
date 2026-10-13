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

#ifndef REMOTE_IFILESYSTEMBACKEND_H
#define REMOTE_IFILESYSTEMBACKEND_H

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <utility>

namespace remote {

// Filesystem-access seam used by the workspace file tree and the editor's
// open/save path. Phase 1 only provides a local (QFile-backed) implementation
// so the seam is real and exercised with zero regression; the remote
// (SFTP-backed) implementation is Phase 2 — RemoteExecutionContext::fsBackend()
// returns nullptr with a `// TODO P2` until then.
//
// Kept deliberately small: stat / read / write / list + a directoryChanged
// signal. Anything richer (rename, mkdir, watch granularity) is added when the
// Phase 2 remote tree needs it.
struct FileStat
{
    bool      exists = false;
    bool      isDir = false;
    qint64    size = 0;
    QDateTime lastModified;
};

class IFileSystemBackend : public QObject
{
    Q_OBJECT

public:
    explicit IFileSystemBackend(QObject *parent = nullptr) : QObject(parent) {}
    ~IFileSystemBackend() override = default;

    // True for an SFTP-backed remote backend, false for the QFile-backed local
    // one. The editor's async open/save path keys off this to decide whether a
    // buffer is a remote (ssh://) buffer; see ScintillaNext::isRemote().
    virtual bool isRemote() const { return false; }

    // Result-delivery callbacks for the async API. Invoked exactly once. `ok` is
    // the I/O-success flag; `error` carries a human reason when !ok. Defined on
    // the interface so a caller (editor open/save) can use one async contract
    // over both backends: the local QFile-backed backend fires them SYNCHRONOUSLY
    // inline (so a local open/save behaves exactly as a blocking call), the
    // remote SFTP backend posts to its worker and fires them later on the UI
    // thread.
    using ReadCallback = std::function<void(bool ok, const QByteArray &data, const QString &error)>;
    using WriteCallback = std::function<void(bool ok, const QString &error)>;

    // Async read/write. The default implementations delegate to the synchronous
    // readFile/writeFile and fire the callback inline — correct and zero-cost for
    // the local backend. The remote backend overrides them to go through its
    // worker thread (never blocking the UI on the network).
    virtual void readFileAsync(const QString &path, const ReadCallback &cb)
    {
        bool ok = false;
        QByteArray data = readFile(path, &ok);
        if (cb) cb(ok, data, ok ? QString() : QStringLiteral("read failed"));
    }
    virtual void writeFileAsync(const QString &path, const QByteArray &data, const WriteCallback &cb)
    {
        const bool ok = writeFile(path, data);
        if (cb) cb(ok, ok ? QString() : QStringLiteral("write failed"));
    }

    // Synchronous read/write/stat. `ok` (when non-null) reports success vs an
    // I/O error (as opposed to a simply-absent file, which is not an error for
    // stat()). Remote (P2) implementations may add async variants alongside.
    virtual QByteArray readFile(const QString &path, bool *ok = nullptr) = 0;
    virtual bool writeFile(const QString &path, const QByteArray &data) = 0;
    virtual FileStat stat(const QString &path) = 0;
    virtual QStringList readdir(const QString &path) = 0;

    // Diagnostic hook. Appends `message` to whatever log the backend maintains
    // (for the remote backend: the SSH connection's rolling debug log; for the
    // local backend: no-op). Called by ScintillaNext to stamp the load-timeout
    // event directly into the SSH log so it can be correlated with worker state.
    virtual void logEvent(const QString &) {}

    // Like readFileAsync, returning a reqId for cancelReadAsync. 0 means there
    // is nothing to cancel: the default reads untracked (the local backend
    // completes inline).
    virtual quint64 readFileAsyncTracked(const QString &path, const ReadCallback &cb)
    {
        readFileAsync(path, cb);
        return 0;
    }

    // Cancel a pending async read by reqId. No-op if the op has already
    // completed or the backend is local. Safe to call after timeout fires.
    virtual void cancelReadAsync(quint64 /*reqId*/) {}

signals:
    // Emitted when a watched directory's contents change. Local backend wires
    // this to a filesystem watcher; remote (P2) derives it from SFTP polling
    // or server notifications.
    void directoryChanged(const QString &path);
};

} // namespace remote

#endif // REMOTE_IFILESYSTEMBACKEND_H
