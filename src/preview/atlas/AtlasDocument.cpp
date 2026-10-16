#include "AtlasDocument.h"

#include <QPainter>

#include <algorithm>

namespace Atlas {

void Document::indexFont()
{
    glyphIndex.clear();
    kerningAmount.clear();
    glyphIndex.reserve(int(glyphs.size()));
    for (size_t i = 0; i < glyphs.size(); ++i)
        glyphIndex.insert(glyphs[i].id, int(i));
    kerningAmount.reserve(int(kernings.size()));
    for (const Kerning &k : kernings)
        kerningAmount.insert((quint64(k.first) << 32) | k.second, k.amount);
}

QImage renderBitmapFont(const Document &font, const std::vector<QImage> &pages, const QString &text, int maxWidth)
{
    if (font.kind != Document::Kind::BitmapFont || text.isEmpty())
        return {};
    const int lineHeight = std::max(1, font.lineHeight);

    // Layout pass: pen positions per glyph, line by line.
    struct Placed {
        const Glyph *glyph;
        int x, y;
    };
    std::vector<Placed> placed;
    const QList<uint> chars = text.toUcs4();
    int penX = 0, penY = 0, width = 0;
    quint32 previous = 0;
    int lastSpace = -1;          // index into `placed` after the last space
    int lastSpaceX = 0;          // pen position there
    for (const uint ch : chars) {
        if (ch == '\n') {
            width = std::max(width, penX);
            penX = 0;
            penY += lineHeight;
            previous = 0;
            lastSpace = -1;
            continue;
        }
        const auto it = font.glyphIndex.constFind(ch);
        if (it == font.glyphIndex.constEnd()) {
            previous = 0;
            continue;
        }
        const Glyph &g = font.glyphs[size_t(it.value())];
        if (previous)
            penX += font.kerningAmount.value((quint64(previous) << 32) | ch, 0);
        if (maxWidth > 0 && penX + g.xadvance > maxWidth && ch != ' ' && penX > 0) {
            // Wrap at the last space when there is one on this line.
            if (lastSpace >= 0) {
                const int shift = lastSpaceX;
                for (size_t k = size_t(lastSpace); k < placed.size(); ++k) {
                    placed[k].x -= shift;
                    placed[k].y += lineHeight;
                }
                penX -= shift;
                penY += lineHeight;
                lastSpace = -1;
            } else {
                width = std::max(width, penX);
                penX = 0;
                penY += lineHeight;
            }
        }
        placed.push_back({&g, penX, penY});
        penX += g.xadvance;
        previous = ch;
        if (ch == ' ') {
            lastSpace = int(placed.size());
            lastSpaceX = penX;
        }
        width = std::max(width, penX);
    }
    if (placed.empty())
        return {};

    const int height = penY + lineHeight;
    constexpr int kMaxSide = 8192;
    if (width <= 0 || width > kMaxSide || height > kMaxSide)
        return {};
    QImage out(width + 2, height, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    for (const Placed &pl : placed) {
        const Glyph &g = *pl.glyph;
        if (g.page < 0 || size_t(g.page) >= pages.size() || pages[size_t(g.page)].isNull() || g.rect.isEmpty())
            continue;
        p.drawImage(QPoint(pl.x + g.xoffset, pl.y + g.yoffset), pages[size_t(g.page)], g.rect);
    }
    return out;
}

} // namespace Atlas
