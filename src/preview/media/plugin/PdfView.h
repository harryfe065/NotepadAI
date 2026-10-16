#ifndef PDFVIEW_H
#define PDFVIEW_H

#include "MediaPluginInterface.h"

#include <QBuffer>
#include <QFutureWatcher>
#include <QWidget>

class QLabel;
class QPdfDocument;
class QPdfView;
class QSpinBox;
class QToolButton;

// PDF tab content (add-media-preview D5): QPdfView in continuous mode, page
// navigation, zoom and fit modes, password prompt. Files up to 512 MB are read
// into memory on a worker first, so no handle stays open on them (a handle
// blocks atomic saves on Windows); larger ones are opened from disk.
class PdfView : public QWidget, public MediaPlugin::View
{
    Q_OBJECT

public:
    PdfView(MediaPlugin::Host *host, QWidget *parent = nullptr);
    ~PdfView() override;

    QWidget *widget() override { return this; }
    void load(const QString &path, bool reload) override;
    void setTabVisible(bool visible) override { Q_UNUSED(visible); }
    void themeChanged(const QPalette &palette, bool isDark) override;

    static constexpr qint64 kMaxInMemoryBytes = 512LL * 1024 * 1024;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    struct ReadResult {
        QByteArray bytes;
        QString error;
    };

    void onRead();
    void openDocument();
    void onStatusChanged();
    void askPassword();
    void zoomBy(double factor);
    void setFit(bool width, bool page);
    void syncControls();
    void updateInfo();

    MediaPlugin::Host *m_host;
    QString m_path;
    qint64 m_fileSize = 0;
    QPdfDocument *m_doc = nullptr;
    QPdfView *m_view = nullptr;
    QBuffer m_buffer;
    QFutureWatcher<ReadResult> m_reader;
    bool m_fromDisk = false;

    QToolButton *m_prev = nullptr;
    QSpinBox *m_page = nullptr;
    QLabel *m_pageCount = nullptr;
    QToolButton *m_next = nullptr;
    QToolButton *m_zoomOut = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QToolButton *m_zoomIn = nullptr;
    QToolButton *m_fitWidth = nullptr;
    QToolButton *m_fitPage = nullptr;

    // State carried across a reload.
    int m_restorePage = -1;
    bool m_syncing = false;
};

#endif // PDFVIEW_H
