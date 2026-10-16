#ifndef PREVIEWTABMANAGER_H
#define PREVIEWTABMANAGER_H

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <functional>

#include "PreviewContentWidget.h"
#include "DockWidget.h"
#include "preview/FileKindResolver.h"

class ScintillaNext;
class NotepadNextApplication;
class DockedEditor;
class QFileSystemWatcher;

class PreviewTabManager : public QObject
{
    Q_OBJECT

public:
    struct TypeRegistration {
        QStringList extensions;
        QString iconPath;
        std::function<PreviewContentWidget *(QWidget *parent)> factory;
        // When true, openPreviewFromFile calls PreviewContentWidget::loadFromFile
        // (the widget owns its mmap + size cap) and SKIPS the QByteArray-read +
        // decode + setContent path and the 10 MB cap. Markdown/HTML leave this
        // false and keep the existing decoded-text route.
        bool wantsFilePath = false;
        // Human-readable type label for the toolbar tooltip ("Markdown", "HTML",
        // "CSV", "TSV"). Empty falls back to a generic "Preview" label.
        QString displayName;
        // Scintilla language names (e.g. "Markdown", "HTML") that map to this
        // preview type even when the buffer is UNSAVED and has no file extension.
        // The extension match wins; this is the fallback so an unsaved Markdown
        // buffer with the Markdown lexer stays previewable. wantsFilePath types
        // (CSV/TSV) deliberately leave this empty — they need a real on-disk path.
        QStringList languageNames;
        // Asset types (images, models, media): every open surface (tree click,
        // Ctrl+P, File > Open, drop, CLI, links) opens the preview instead of a
        // text editor. Markdown/HTML/CSV leave this false and open as text.
        bool opensByDefault = false;
        // Reload through PreviewContentWidget::reloadFromDisk() when the file
        // changes, via the manager's one shared watcher (CSV keeps its own).
        bool watchFile = false;
        // Content sniffers for files whose extension is missing or unknown.
        QVector<FileKindResolver::Sniffer> sniffers;
        // Generic extensions (json, xml, plist, txt, ...) whose content decides
        // whether this type can preview the file. Consulted only by the explicit
        // Preview actions (editor toolbar / tree menu), never by plain open, so
        // a .json is never hijacked. The predicate sees the first 4 KB.
        QStringList sniffExtensions;
        FileKindResolver::Sniffer contentSniffer;
    };

    enum class OpenMode { Transient, Pinned };

    struct FilePreviewEntry {
        QString path;
        QString typeId;
    };

    explicit PreviewTabManager(NotepadNextApplication *app, DockedEditor *dockedEditor, QObject *parent = nullptr);

    void registerType(const QString &typeId, const TypeRegistration &reg);
    bool canPreview(const QString &filePath) const;
    // True when filePath's preview type uses the file-path (mmap) route, which
    // enforces its own multi-GB cap inside loadFromFile — callers must NOT apply
    // the 10 MB decoded-text pre-check to these. False for unknown types too.
    bool previewWantsFilePath(const QString &filePath) const;

    // The one routing decision shared by every open surface.
    FileKindResolver &resolver() { return m_resolver; }
    const FileKindResolver &resolver() const { return m_resolver; }
    bool hasType(const QString &typeId) const { return m_registry.contains(typeId); }

    // Editor-aware preview gating, used by the toolbar/menu action. Resolves the
    // active editor to a registered preview type (extension first, Scintilla
    // language name as fallback for unsaved buffers) and exposes the result for
    // the action's enabled-state, dynamic tooltip, and dispatch — so all three
    // read from ONE resolver and can never disagree.
    bool canPreviewEditor(ScintillaNext *editor) const;
    // Human-readable type label for the active editor ("Markdown"/"HTML"/"CSV"/
    // "TSV"), or empty when unknown/unsupported. Drives the dynamic tooltip.
    QString previewTypeName(ScintillaNext *editor) const;

    void openOrFocusPreview(ScintillaNext *sourceEditor);
    // Transient preview from the tree context menu ("Preview").
    void openPreviewFromFile(const QString &filePath);
    // Opens (or focuses) a file preview of `typeId` (empty: by extension).
    // Transient reuses the one italic preview slot shared with the preview
    // editor; Pinned opens a permanent tab (and pins the transient one when it
    // already shows this file). replaceInitialEditor closes a pristine "New X"
    // after the tab is added (DockedEditor::initialEditor ordering rule).
    bool openFilePreview(const QString &filePath, const QString &typeId, OpenMode mode, bool replaceInitialEditor);
    void closePreview(ScintillaNext *sourceEditor);
    PreviewContentWidget *previewForEditor(ScintillaNext *sourceEditor) const;

    // Pinned file previews in the order they were opened (session save), and
    // the path of the preview that was the active tab, if any.
    QVector<FilePreviewEntry> pinnedFilePreviews() const;
    QString activeFilePreviewPath() const;
    // Raises the file preview of `filePath`; false when none is open.
    bool focusFilePreview(const QString &filePath);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void previewOpened(PreviewContentWidget *preview);
    void previewClosed(PreviewContentWidget *preview);
    // An asset preview asked to show its file in a text editor.
    void openAsTextRequested(const QString &filePath);

private:
    struct PreviewEntry {
        QPointer<PreviewContentWidget> widget;
        QPointer<ads::CDockWidget> dockWidget;
        QString iconPath;
        QTimer *debounceTimer = nullptr;
    };

    const TypeRegistration *findRegistration(const QString &filePath) const;
    // Content-sniff fallback of findRegistration for sniffExtensions.
    const TypeRegistration *sniffRegistration(const QString &filePath, const QString &suffixLower) const;

    // sniffRegistration verdicts by path, valid while mtime and size match.
    struct SniffVerdict {
        qint64 modified = 0;
        qint64 size = 0;
        QString typeId;     // empty: no type claims the file
    };
    static constexpr qsizetype kMaxSniffCache = 512;
    mutable QHash<QString, SniffVerdict> m_sniffCache;
    // Resolves an editor to its preview type: file-extension match first, then
    // Scintilla language-name fallback for unsaved buffers. Returns nullptr for
    // a wantsFilePath type on a buffer with no on-disk path (CSV/TSV need mmap),
    // so the gate, tooltip, and dispatch all reject it consistently.
    const TypeRegistration *registrationForEditor(ScintillaNext *editor) const;
    void scheduleRender(ScintillaNext *sourceEditor);
    void performRender(ScintillaNext *sourceEditor);
    void syncScroll(ScintillaNext *sourceEditor);
    int debounceMs(int docLength) const;
    void pinTransientPreviewTab();
    void wirePreviewWidget(PreviewContentWidget *preview, const TypeRegistration &reg, const QString &path);
    void watchPath(const QString &path);
    void unwatchPath(const QString &path);
    void flushChangedFiles();

    NotepadNextApplication *m_app;
    DockedEditor *m_dockedEditor;
    QHash<QString, TypeRegistration> m_registry;
    QHash<QString, QString> m_typeByExtension;
    QSet<QString> m_sniffableExtensions;
    FileKindResolver m_resolver;
    QHash<ScintillaNext *, PreviewEntry> m_previews;
    QHash<QString, QPointer<ads::CDockWidget>> m_previewsByPath;
    // Open order of file-path previews, for session save.
    QVector<QPointer<ads::CDockWidget>> m_filePreviewOrder;
    QPointer<ads::CDockWidget> m_transientPreviewTab;
    QPointer<QWidget> m_activePreviewWidget;

    // One watcher for every watchFile preview; refcounted by path.
    QFileSystemWatcher *m_watcher = nullptr;
    QHash<QString, int> m_watchCount;
    QHash<QString, int> m_changedPending; // path -> ticks waited for it to reappear
    QTimer m_changeTimer;
};

#endif // PREVIEWTABMANAGER_H
