#include "PreviewTabManager.h"
#include "DockedEditor.h"
#include "ScintillaNext.h"
#include "NotepadNextApplication.h"
#include "MarkdownPreviewWidget.h"
#include "HtmlPreviewWidget.h"
#include "FileEncodingDetector.h"

#include "DockWidget.h"
#include "DockWidgetTab.h"
#include "DockAreaWidget.h"

#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QEvent>
#include <QPainter>
#include <QtConcurrent>

#include <ScintillaTypes.h>

static QIcon tintIcon(const QString &svgPath, const QColor &color)
{
    QIcon source(svgPath);
    if (source.isNull()) return source;
    QIcon dst;
    for (const QSize &sz : {QSize(16, 16), QSize(24, 24), QSize(32, 32)}) {
        QPixmap pm = source.pixmap(sz);
        QPainter p(&pm);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(pm.rect(), color);
        p.end();
        dst.addPixmap(pm);
    }
    return dst;
}

PreviewTabManager::PreviewTabManager(NotepadNextApplication *app, DockedEditor *dockedEditor, QObject *parent)
    : QObject(parent)
    , m_app(app)
    , m_dockedEditor(dockedEditor)
{
    connect(app, &NotepadNextApplication::effectiveThemeChanged, this, [this]() {
        QPalette pal = m_app->palette();
        bool isDark = m_app->isEffectiveThemeDark();
        QColor iconColor = pal.color(QPalette::ButtonText);
        // Editor-bound previews (Markdown/HTML live previews, etc.).
        for (auto &entry : m_previews) {
            if (entry.widget)
                entry.widget->applyTheme(pal, isDark);
            if (entry.dockWidget && !entry.iconPath.isEmpty())
                entry.dockWidget->tabWidget()->setIcon(tintIcon(entry.iconPath, iconColor));
        }
        // File-path previews (the route CSV uses; also Markdown/HTML opened from
        // the file tree). Without this loop these tabs never re-theme on a
        // light/dark switch. The iconPath is recovered from the registry by the
        // widget's typeId() (CSV/TSV share the csv icon).
        for (auto it = m_previewsByPath.begin(); it != m_previewsByPath.end(); ++it) {
            ads::CDockWidget *dock = it.value();
            if (!dock) continue;
            auto *preview = qobject_cast<PreviewContentWidget *>(dock->widget());
            if (!preview) continue;
            preview->applyTheme(pal, isDark);
            // Recover the icon path from the registry by typeId (CSV/TSV both
            // report "csv" and share the csv icon, so the lookup is correct).
            auto regIt = m_registry.constFind(preview->typeId());
            if (regIt != m_registry.constEnd() && !regIt.value().iconPath.isEmpty())
                dock->tabWidget()->setIcon(tintIcon(regIt.value().iconPath, iconColor));
        }
    });

    connect(dockedEditor, &DockedEditor::previewEditorSet, this, [this]() {
        if (m_transientPreviewTab) {
            m_transientPreviewTab->closeDockWidget();
            m_transientPreviewTab = nullptr;
        }
    });
}

void PreviewTabManager::registerType(const QString &typeId, const TypeRegistration &reg)
{
    m_registry.insert(typeId, reg);
}

const PreviewTabManager::TypeRegistration *PreviewTabManager::findRegistration(const QString &filePath) const
{
    QString ext = QFileInfo(filePath).suffix().toLower();
    for (auto it = m_registry.constBegin(); it != m_registry.constEnd(); ++it) {
        if (it.value().extensions.contains(ext))
            return &it.value();
    }
    return nullptr;
}

bool PreviewTabManager::canPreview(const QString &filePath) const
{
    return findRegistration(filePath) != nullptr;
}

bool PreviewTabManager::previewWantsFilePath(const QString &filePath) const
{
    const TypeRegistration *reg = findRegistration(filePath);
    return reg && reg->wantsFilePath;
}

const PreviewTabManager::TypeRegistration *PreviewTabManager::registrationForEditor(ScintillaNext *editor) const
{
    if (!editor) return nullptr;

    // 1) Extension match (authoritative when the buffer is backed by a file).
    const TypeRegistration *reg = nullptr;
    if (editor->isFile())
        reg = findRegistration(editor->getFilePath());

    // 2) Fallback: map the active Scintilla language name onto a preview type so
    //    an UNSAVED buffer (no extension yet) still previews. wantsFilePath types
    //    register no languageNames, so they never resolve through this branch.
    if (!reg) {
        const QString lang = editor->languageName;
        if (!lang.isEmpty()) {
            for (auto it = m_registry.constBegin(); it != m_registry.constEnd(); ++it) {
                if (it.value().languageNames.contains(lang)) {
                    reg = &it.value();
                    break;
                }
            }
        }
    }

    if (!reg) return nullptr;

    // A file-path-route type (CSV/TSV mmaps a real file) cannot preview a buffer
    // with no on-disk path — loadFromFile would have nothing to open. Reject so
    // the action is disabled rather than opening an empty grid.
    if (reg->wantsFilePath && !editor->isFile())
        return nullptr;

    return reg;
}

bool PreviewTabManager::canPreviewEditor(ScintillaNext *editor) const
{
    return registrationForEditor(editor) != nullptr;
}

QString PreviewTabManager::previewTypeName(ScintillaNext *editor) const
{
    const TypeRegistration *reg = registrationForEditor(editor);
    return reg ? reg->displayName : QString();
}

int PreviewTabManager::debounceMs(int docLength) const
{
    if (docLength < 10 * 1024)  return 150;
    if (docLength < 100 * 1024) return 300;
    return 800;
}

void PreviewTabManager::openOrFocusPreview(ScintillaNext *sourceEditor)
{
    if (!sourceEditor) return;

    auto it = m_previews.find(sourceEditor);
    if (it != m_previews.end() && it->widget) {
        if (it->dockWidget)
            it->dockWidget->raise();
        it->widget->setFocus();
        return;
    }

    // Single resolver — same one the toolbar gate/tooltip use, so a click can
    // never reach here for a type the action claimed was unpreviewable.
    const TypeRegistration *reg = registrationForEditor(sourceEditor);
    if (!reg) return;

    PreviewContentWidget *preview = reg->factory(nullptr);
    if (!preview) return;

    preview->applyTheme(m_app->palette(), m_app->isEffectiveThemeDark());

    QString basePath;
    if (sourceEditor->isFile())
        basePath = QFileInfo(sourceEditor->getFilePath()).absolutePath();

    if (reg->wantsFilePath) {
        // File-path route (CSV/TSV): the widget owns mmap + its own size cap.
        // registrationForEditor() already guaranteed isFile(), so the path is
        // valid — no empty-grid / null-path case reaches here.
        preview->loadFromFile(sourceEditor->getFilePath());
    } else {
        QString text = QString::fromUtf8(sourceEditor->getText(sourceEditor->length() + 1));
        preview->setContent(text, basePath);
    }

    QString title = sourceEditor->isFile()
        ? QFileInfo(sourceEditor->getFilePath()).fileName()
        : sourceEditor->getName();

    ads::CDockWidget *dockWidget = m_dockedEditor->addPreviewTab(
        preview, title, tintIcon(reg->iconPath, m_app->palette().color(QPalette::ButtonText)));

    PreviewEntry entry;
    entry.widget = preview;
    entry.dockWidget = dockWidget;
    entry.iconPath = reg->iconPath;
    entry.debounceTimer = new QTimer(this);
    entry.debounceTimer->setSingleShot(true);
    m_previews.insert(sourceEditor, entry);

    connect(entry.debounceTimer, &QTimer::timeout, this, [this, sourceEditor]() {
        performRender(sourceEditor);
    });

    // Live update on content change. Only for the decoded-text route: a
    // wantsFilePath preview (CSV/TSV) reads from disk via its own watcher and
    // owns its mmap — copying the entire editor buffer on every keystroke would
    // defeat that and is wasted work, so we skip the hookup entirely for it.
    if (!reg->wantsFilePath) {
        connect(sourceEditor, &ScintillaNext::updateUi, this, [this, sourceEditor](Scintilla::Update updated) {
            if (!m_previews.contains(sourceEditor)) return;
            if (Scintilla::FlagSet(updated, Scintilla::Update::Content))
                scheduleRender(sourceEditor);
            else if (Scintilla::FlagSet(updated, Scintilla::Update::VScroll))
                syncScroll(sourceEditor);
        });
    }

    // Lifecycle: close preview when source editor is destroyed
    connect(sourceEditor, &QObject::destroyed, this, [this, sourceEditor]() {
        auto it = m_previews.find(sourceEditor);
        if (it != m_previews.end()) {
            if (it->dockWidget)
                it->dockWidget->closeDockWidget();
            delete it->debounceTimer;
            m_previews.erase(it);
        }
    });

    // Cleanup hash when preview widget is destroyed (e.g. user closes tab)
    connect(preview, &QObject::destroyed, this, [this, sourceEditor]() {
        auto it = m_previews.find(sourceEditor);
        if (it != m_previews.end()) {
            delete it->debounceTimer;
            m_previews.erase(it);
        }
    });

    // Update title on rename
    connect(sourceEditor, &ScintillaNext::renamed, this, [this, sourceEditor, wantsFilePath = reg->wantsFilePath]() {
        auto it = m_previews.find(sourceEditor);
        if (it == m_previews.end() || !it->widget || !it->dockWidget) return;
        QString newTitle = sourceEditor->isFile()
            ? QFileInfo(sourceEditor->getFilePath()).fileName()
            : sourceEditor->getName();
        it->dockWidget->setWindowTitle(newTitle);
        if (wantsFilePath) {
            // File-path route: re-point the widget at the new on-disk path.
            if (sourceEditor->isFile())
                it->widget->loadFromFile(sourceEditor->getFilePath());
        } else {
            QString text = QString::fromUtf8(sourceEditor->getText(sourceEditor->length() + 1));
            QString basePath;
            if (sourceEditor->isFile())
                basePath = QFileInfo(sourceEditor->getFilePath()).absolutePath();
            it->widget->setContent(text, basePath);
        }
    });

    emit previewOpened(preview);
}

void PreviewTabManager::closePreview(ScintillaNext *sourceEditor)
{
    auto it = m_previews.find(sourceEditor);
    if (it == m_previews.end()) return;
    if (it->dockWidget)
        it->dockWidget->closeDockWidget();
}

PreviewContentWidget *PreviewTabManager::previewForEditor(ScintillaNext *sourceEditor) const
{
    auto it = m_previews.constFind(sourceEditor);
    if (it != m_previews.constEnd())
        return it->widget.data();
    return nullptr;
}

void PreviewTabManager::scheduleRender(ScintillaNext *sourceEditor)
{
    auto it = m_previews.find(sourceEditor);
    if (it == m_previews.end()) return;
    int ms = debounceMs(static_cast<int>(sourceEditor->length()));
    it->debounceTimer->start(ms);
}

void PreviewTabManager::performRender(ScintillaNext *sourceEditor)
{
    auto it = m_previews.find(sourceEditor);
    if (it == m_previews.end() || !it->widget) return;

    PreviewContentWidget *widget = it->widget;
    QString text = QString::fromUtf8(sourceEditor->getText(sourceEditor->length() + 1));
    QString basePath;
    if (sourceEditor->isFile())
        basePath = QFileInfo(sourceEditor->getFilePath()).absolutePath();

    widget->refresh(text);
}

void PreviewTabManager::syncScroll(ScintillaNext *sourceEditor)
{
    auto it = m_previews.find(sourceEditor);
    if (it == m_previews.end() || !it->widget) return;

    auto *mdWidget = qobject_cast<MarkdownPreviewWidget *>(it->widget.data());
    if (mdWidget) {
        int firstLine = static_cast<int>(sourceEditor->firstVisibleLine()) + 1;
        mdWidget->scrollToLine(firstLine);
    }
}

void PreviewTabManager::openPreviewFromFile(const QString &filePath)
{
    const TypeRegistration *reg = findRegistration(filePath);
    if (!reg) return;

    QFileInfo fi(filePath);
    // The 10 MB cap applies only to the decoded-text route. File-path-route
    // previews (CSV) enforce their own (much larger) cap inside loadFromFile.
    if (!reg->wantsFilePath && fi.size() > 10 * 1024 * 1024) return;

    // Normalized key for the path→dock registry. Use absoluteFilePath (not
    // canonicalFilePath) so the key is well-defined even before the file is
    // resolved through symlinks, matching what the source path carries.
    const QString pathKey = fi.absoluteFilePath();

    // Already previewing this exact file (transient or pinned): focus it
    // instead of creating a duplicate tab. Mirrors openOrFocusPreview().
    auto existing = m_previewsByPath.find(pathKey);
    if (existing != m_previewsByPath.end()) {
        if (existing.value()) {
            existing.value()->raise();
            existing.value()->setAsCurrentTab();
            existing.value()->widget()->setFocus();
            return;
        }
        // Dock died without our destroyed-handler firing yet; drop the stale key.
        m_previewsByPath.erase(existing);
    }

    QString text;
    if (!reg->wantsFilePath) {
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly)) return;
        QByteArray raw = f.readAll();
        f.close();

        if (raw.left(512).contains('\0')) return;

        FileEncodingDetector::decode(raw, text);
    }

    // Close editor preview tab if one exists (shared transient slot)
    if (m_dockedEditor->previewEditor())
        m_dockedEditor->previewEditor()->close();

    // Close existing transient preview tab (replaceable, like editor preview tab)
    if (m_transientPreviewTab) {
        m_transientPreviewTab->closeDockWidget();
        m_transientPreviewTab = nullptr;
    }

    PreviewContentWidget *preview = reg->factory(nullptr);
    if (!preview) return;

    preview->applyTheme(m_app->palette(), m_app->isEffectiveThemeDark());

    QString basePath = fi.absolutePath();
    if (reg->wantsFilePath) {
        // File-path route: the widget owns mmap + its own cap. No decoded text.
        preview->loadFromFile(filePath);
    } else {
        preview->setContent(text, basePath);

        auto *htmlPreview = qobject_cast<HtmlPreviewWidget *>(preview);
        if (htmlPreview)
            htmlPreview->setFilePath(filePath);
    }

    QString title = fi.fileName();
    QColor iconColor = m_app->palette().color(QPalette::ButtonText);
    ads::CDockWidget *dockWidget = m_dockedEditor->addPreviewTab(
        preview, title, tintIcon(reg->iconPath, iconColor));
    dockWidget->tabWidget()->setToolTip(filePath);

    // Italic style to indicate transient/preview state
    QFont tabFont = dockWidget->tabWidget()->font();
    tabFont.setItalic(true);
    dockWidget->tabWidget()->setFont(tabFont);

    m_transientPreviewTab = dockWidget;
    m_previewsByPath.insert(pathKey, dockWidget);

    // Double-click the tab to promote it from transient to a permanent tab,
    // mirroring DockedEditor's preview-editor pin behavior.
    dockWidget->tabWidget()->installEventFilter(this);

    // Clean up registry/transient-slot when this specific dock is destroyed
    // (user closes the tab, or it's replaced by the next transient preview).
    // The connection is scoped to dockWidget as context object, so it is torn
    // down with the dock — preventing dead lambdas from piling up across opens.
    connect(dockWidget, &QObject::destroyed, dockWidget, [this, pathKey, dockWidget]() {
        if (m_transientPreviewTab == dockWidget)
            m_transientPreviewTab = nullptr;
        auto it = m_previewsByPath.find(pathKey);
        if (it != m_previewsByPath.end() && it.value() == dockWidget)
            m_previewsByPath.erase(it);
    });

    // Let the preview drive its own tab title (e.g. the CSV preview prefixes a
    // dirty marker when its edit overlay is non-empty). Scoped to the dock so it
    // tears down with the tab.
    connect(preview, &PreviewContentWidget::titleChanged, dockWidget,
            [dockWidget](const QString &t) {
        if (dockWidget) dockWidget->setWindowTitle(t);
    });

    emit previewOpened(preview);
}

void PreviewTabManager::pinTransientPreviewTab()
{
    if (!m_transientPreviewTab) return;

    auto *tab = m_transientPreviewTab->tabWidget();
    QFont f = tab->font();
    f.setItalic(false);
    tab->setFont(f);
    tab->removeEventFilter(this);

    // No longer transient: it survives the next preview instead of being replaced.
    m_transientPreviewTab = nullptr;
}

bool PreviewTabManager::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonDblClick && m_transientPreviewTab) {
        auto *tab = qobject_cast<ads::CDockWidgetTab *>(watched);
        if (tab && tab->dockWidget() == m_transientPreviewTab) {
            pinTransientPreviewTab();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
