#ifndef ATLASDOCUMENT_H
#define ATLASDOCUMENT_H

#include <QHash>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

// A texture atlas (sprite sheet) or a bitmap font: named rectangles on one or
// more page images. Filled by AtlasLoader from libGDX / Spine .atlas,
// TexturePacker and Aseprite JSON, Godot .tpsheet, Starling / Sparrow XML,
// Cocos2d plists and BMFont text / XML / binary files.
namespace Atlas {

struct Page {
    QString file;       // as written in the atlas
    QString path;       // resolved absolute path ("" when not found)
    QSize size;         // as declared (may be empty)
};

struct Frame {
    QString name;
    QString tag;                // Aseprite animation tag, when the sheet has them
    int page = 0;
    QRect rect;                 // on the page, as stored (already swapped when rotated)
    bool rotated = false;       // stored turned 90 degrees
    QSize sourceSize;           // untrimmed size
    QPoint trim;                // offset of the stored pixels inside the source
    QPointF pivot;
    bool hasPivot = false;
    int index = -1;             // libGDX "index" for animation sequences
};

struct Glyph {
    quint32 id = 0;             // Unicode code point
    int page = 0;
    QRect rect;
    int xoffset = 0, yoffset = 0, xadvance = 0;
};

struct Kerning {
    quint32 first = 0, second = 0;
    int amount = 0;
};

struct Document {
    enum class Kind { Atlas, BitmapFont };

    Kind kind = Kind::Atlas;
    QString format;             // "libGDX atlas", "BMFont (binary)", ...
    std::vector<Page> pages;
    std::vector<Frame> frames;

    // Bitmap fonts.
    QString face;
    int fontSize = 0;
    int lineHeight = 0;
    int base = 0;
    std::vector<Glyph> glyphs;
    std::vector<Kerning> kernings;
    QHash<quint32, int> glyphIndex;       // code point -> glyphs index
    QHash<quint64, int> kerningAmount;    // (first << 32 | second) -> amount

    QStringList warnings;

    int itemCount() const
    {
        return int(kind == Kind::Atlas ? frames.size() : glyphs.size());
    }
    // Fills glyphIndex and kerningAmount after glyphs / kernings are set.
    void indexFont();
};

using DocumentPtr = std::shared_ptr<const Document>;

// Draws `text` with the font's glyphs (advance, offsets and kerning) on a
// transparent image; lines break at '\n' and, past `maxWidth` (0 = never), at
// the last space. Characters the font lacks are skipped. Null when nothing
// was drawn.
QImage renderBitmapFont(const Document &font, const std::vector<QImage> &pages, const QString &text, int maxWidth);

} // namespace Atlas

#endif // ATLASDOCUMENT_H
