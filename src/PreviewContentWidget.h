#ifndef PREVIEWCONTENTWIDGET_H
#define PREVIEWCONTENTWIDGET_H

#include <QWidget>
#include <QPalette>

class PreviewContentWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PreviewContentWidget(QWidget *parent = nullptr) : QWidget(parent) {}

    virtual QString typeId() const = 0;
    virtual QString displayName() const = 0;
    virtual void setContent(const QString &text, const QString &basePath) = 0;
    virtual void refresh(const QString &text) = 0;
    virtual void applyTheme(const QPalette &palette, bool isDark) = 0;

    // File-path load route. Default no-op: previews that opt in via the
    // TypeRegistration `wantsFilePath` flag (e.g. the CSV table preview) override
    // this to own their loading (mmap, their own size cap) and bypass the
    // QByteArray-read + decode + setContent path. Markdown/HTML do not override.
    virtual void loadFromFile(const QString &path) { Q_UNUSED(path); }

    // The file changed on disk (PreviewTabManager's shared watcher, for types
    // registered with `watchFile`). Asset viewers reload keeping their view.
    virtual void reloadFromDisk() {}

signals:
    void titleChanged(const QString &title);
};

#endif // PREVIEWCONTENTWIDGET_H
