#ifndef FONTPREVIEWWIDGET_H
#define FONTPREVIEWWIDGET_H

#include "AssetPreviewWidget.h"
#include "FontFile.h"

#include <QFutureWatcher>
#include <QRawFont>

class FontSampleView;
class GlyphGridView;
class QComboBox;
class QLineEdit;

// Font tab (add-media-preview D6): face picker, editable sample at several
// sizes, virtualised glyph grid by Unicode block, metadata from the font's
// tables. The font is read and parsed on a worker and rendered with QRawFont
// only: it is never registered with the application or the system.
class FontPreviewWidget : public AssetPreviewWidget
{
    Q_OBJECT

public:
    FontPreviewWidget(const QString &typeId, const QString &displayName, QWidget *parent = nullptr);

protected:
    void startLoad(bool reload) override;

private:
    struct Loaded {
        FontFile::Document doc;
        int face = 0;
        QByteArray faceBytes;   // standalone sfnt of `face`, built on the worker
    };
    struct FaceBuild {
        quint64 serial = 0;     // m_loadSerial when started: a reload voids it
        int face = 0;
        QByteArray bytes;
    };

    void onLoaded();
    void selectFace(int index);
    void applyFace(int index, const QByteArray &bytes);
    void rebuildSources();
    void applySource();
    void updateInfo();
    void showHover(int index);
    QString defaultSample() const;

    QFutureWatcher<Loaded> m_watcher;
    QFutureWatcher<FaceBuild> m_faceWatcher;    // collection face rebuilds
    quint64 m_loadSerial = 0;
    FontFile::Document m_doc;
    QRawFont m_raw;
    int m_face = -1;
    bool m_pendingReload = false;
    qint64 m_fileSize = 0;
    QString m_infoText;

    QComboBox *m_faces = nullptr;
    QLineEdit *m_sample = nullptr;
    QComboBox *m_source = nullptr;
    FontSampleView *m_sampleView = nullptr;
    GlyphGridView *m_grid = nullptr;
};

#endif // FONTPREVIEWWIDGET_H
