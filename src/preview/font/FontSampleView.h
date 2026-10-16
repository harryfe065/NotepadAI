#ifndef FONTSAMPLEVIEW_H
#define FONTSAMPLEVIEW_H

#include <QRawFont>
#include <QWidget>

#include <vector>

// The sample text of the font preview at 12-72 px (add-media-preview D6).
// Glyphs come from QRawFont with the font's kerning (no complex shaping: that
// needs the font in the application font database). Glyph ids and advances
// are computed once per text / font change, not per paint.
class FontSampleView : public QWidget
{
    Q_OBJECT

public:
    explicit FontSampleView(QWidget *parent = nullptr);

    void setRawFont(const QRawFont &font);
    void setText(const QString &text);
    QString text() const { return m_text; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Line {
        int pixelSize = 0;
        QRawFont font;
        QList<quint32> glyphs;
        QList<QPointF> positions;   // baseline-relative pen positions
        double ascent = 0, descent = 0;
    };

    void rebuild();

    QRawFont m_font;
    QString m_text;
    std::vector<Line> m_lines;
    int m_height = 0;
};

#endif // FONTSAMPLEVIEW_H
