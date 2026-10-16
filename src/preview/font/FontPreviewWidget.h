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
    void onLoaded();
    void selectFace(int index);
    void rebuildSources();
    void applySource();
    void updateInfo();
    void showHover(int index);
    QString defaultSample() const;

    QFutureWatcher<FontFile::Document> m_watcher;
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
