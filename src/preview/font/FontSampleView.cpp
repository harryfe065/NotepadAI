#include "FontSampleView.h"

#include <QGlyphRun>
#include <QPainter>
#include <QPaintEvent>

#include <cmath>

namespace {

constexpr int kSizes[] = {12, 16, 24, 32, 48, 72};
constexpr int kLabelWidth = 36;
constexpr int kMargin = 12;
constexpr int kLineGap = 6;

} // namespace

FontSampleView::FontSampleView(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void FontSampleView::setRawFont(const QRawFont &font)
{
    m_font = font;
    rebuild();
}

void FontSampleView::setText(const QString &text)
{
    if (text == m_text)
        return;
    m_text = text;
    rebuild();
}

void FontSampleView::rebuild()
{
    m_lines.clear();
    m_height = kMargin;
    if (m_font.isValid() && !m_text.isEmpty()) {
        for (int size : kSizes) {
            Line line;
            line.pixelSize = size;
            line.font = m_font;
            line.font.setPixelSize(size);
            line.glyphs = line.font.glyphIndexesForString(m_text);
            const QList<QPointF> advances = line.font.advancesForGlyphIndexes(line.glyphs, QRawFont::KernedAdvances);
            line.positions.reserve(advances.size());
            QPointF pen;
            for (const QPointF &advance : advances) {
                line.positions.append(pen);
                pen += advance;
            }
            line.ascent = line.font.ascent();
            line.descent = line.font.descent();
            m_height += int(std::ceil(line.ascent + line.descent)) + kLineGap;
            m_lines.push_back(std::move(line));
        }
    }
    m_height += kMargin;
    updateGeometry();
    setFixedHeight(m_height);
    update();
}

QSize FontSampleView::sizeHint() const
{
    return {400, m_height};
}

void FontSampleView::paintEvent(QPaintEvent *event)
{
    QPainter p(this);
    const QPalette &pal = palette();
    p.fillRect(event->rect(), pal.color(QPalette::Base));
    double y = kMargin;
    QFont label = font();
    label.setPixelSize(10);
    p.setFont(label);
    for (const Line &line : m_lines) {
        const double baseline = y + line.ascent;
        const double bottom = baseline + line.descent;
        if (bottom >= event->rect().top() && y <= event->rect().bottom()) {
            p.setPen(pal.color(QPalette::PlaceholderText));
            p.drawText(QRectF(kMargin, baseline - 12, kLabelWidth - 8, 14), Qt::AlignRight | Qt::AlignBottom,
                       QString::number(line.pixelSize));
            QGlyphRun run;
            run.setRawFont(line.font);
            run.setGlyphIndexes(line.glyphs);
            run.setPositions(line.positions);
            p.setPen(pal.color(QPalette::Text));
            p.setClipRect(QRectF(kMargin + kLabelWidth, y, width() - kMargin * 2 - kLabelWidth, bottom - y + 1));
            p.drawGlyphRun(QPointF(kMargin + kLabelWidth, baseline), run);
            p.setClipping(false);
        }
        y = bottom + kLineGap;
    }
}
