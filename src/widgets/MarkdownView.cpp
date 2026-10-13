/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "MarkdownView.h"

#include "markdown/MarkdownPreprocessor.h"
#include "markdown/MathRenderCache.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QEvent>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QIcon>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextTable>
#include <QToolButton>
#include <QUrl>

#include <cmath>
#include <cstdint>

namespace {

// SVG icons that declare stroke="currentColor" resolve to opaque black under
// Qt's svg icon engine, so they vanish on dark backgrounds. Re-render at the
// sizes Qt asks for and tint each pixmap via SourceIn so the alpha (strokes) is
// kept while the rgb becomes the palette color. Mirrors AcpSessionView's local
// tintedIcon (deliberately duplicated per-file, like makeTintedIcon elsewhere).
QIcon tintedCodeIcon(const QString &svgPath, const QColor &color)
{
    QIcon source(svgPath);
    if (source.isNull()) return source;
    QIcon dst;
    for (int sz : {16, 20, 22, 24}) {
        QPixmap pm = source.pixmap(sz, sz);
        if (pm.isNull()) continue;
        QPainter p(&pm);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(pm.rect(), color);
        p.end();
        dst.addPixmap(pm);
    }
    return dst;
}

// QTextBlock carries an implicit top/bottom margin (paragraph leading) that
// `documentMargin(0)` does NOT zero. The result is that `doc->size().height()`
// over-reports the rendered text height by ~one line's worth of leading,
// inflating bubbles with empty space below the last visible line. Walk every
// block and merge a zero-margin block format so the document's size matches
// what the user actually sees.
//
// Exception: fenced code blocks carry a block background (our <pre> fill) and
// deliberately keep vertical margins for breathing room around the code
// surface — zeroing them would glue the block to adjacent prose. Leave any
// background-bearing block's margins untouched.
//
// Exception 2: a block holding only a display-math image gets a small fixed
// margin so the formula does not touch the text above and below it.
bool isImageOnlyBlock(const QTextBlock &b);
void normalizeBlockMarginsImpl(QTextDocument *doc)
{
    if (!doc) return;
    QTextBlockFormat zero;
    zero.setTopMargin(0);
    zero.setBottomMargin(0);
    QTextBlockFormat displayMath;
    displayMath.setTopMargin(4);
    displayMath.setBottomMargin(4);
    QTextCursor cur(doc);
    cur.movePosition(QTextCursor::Start);
    do {
        if (cur.blockFormat().background().style() != Qt::NoBrush) {
            continue; // code surface — preserve its top/bottom margins
        }
        if (isImageOnlyBlock(cur.block())) {
            cur.mergeBlockFormat(displayMath);
            continue;
        }
        // The zero-width paragraph the preprocessor puts between back-to-back
        // fences only has to keep them apart: collapse it to a hair.
        if (cur.block().text() == QString(QChar(0x200B))) {
            QTextBlockFormat separator = zero;
            separator.setLineHeight(1, QTextBlockFormat::FixedHeight);
            cur.mergeBlockFormat(separator);
            continue;
        }
        // Merge on the cursor's own block. (select(BlockUnderCursor) also takes
        // the paragraph separator before the block, which belongs to the PREVIOUS
        // block — that zeroed the bottom margin of a code block above any prose.)
        cur.mergeBlockFormat(zero);
    } while (cur.movePosition(QTextCursor::NextBlock));
}

// Windows (and other platforms) resolve CSS "monospace" to the system fixed
// family and report fontFixedPitch()==false, so after the toHtml -> setHtml round
// trip a code fragment cannot be recognised by family name ("Courier New" there).
// Relabel the baked families with a marker right after toHtml(), while the text
// is still unambiguous, and detect code by the marker.
constexpr const char *kCodeFamilyMarker = "NotepadAICodeFace";

void markCodeFamilies(QString &html, const QString &proseFamily)
{
    const QString fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    const QString marker = QString::fromLatin1(kCodeFamilyMarker);
    for (const QString &family : {QStringLiteral("monospace"), fixed}) {
        // a fixed-pitch chat font would relabel the prose too: leave that alone
        if (family.isEmpty() || family.compare(proseFamily, Qt::CaseInsensitive) == 0)
            continue;
        html.replace(QStringLiteral("font-family:'%1'").arg(family),
                     QStringLiteral("font-family:'%1'").arg(marker));
    }
}

// Code BLOCKS (fenced or indented) carry their own marker family. Only the
// markdown import knows a block from an inline span (BlockCodeLanguage, set on
// every line of either kind of block); the HTML round trip drops it. Without
// this, a paragraph or list item holding nothing but an inline span
// (- `src/a.cpp`) reads as a code block: surface, padding, the lot.
constexpr const char *kCodeBlockFamilyMarker = "NotepadAICodeBlockFace";

void markCodeBlocks(QTextDocument &doc)
{
    QTextCharFormat marker;
    marker.setFontFamilies({QString::fromLatin1(kCodeBlockFamilyMarker)});
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        if (b.length() <= 1 || !b.blockFormat().hasProperty(QTextFormat::BlockCodeLanguage))
            continue;
        QTextCursor cur(b);
        cur.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        cur.mergeCharFormat(marker);
    }
}

// Family for code: the chat font when it is already fixed-pitch, else the best
// installed programming font, else the system fixed font. (A proportional face
// makes code blocks hard to tell from prose and misaligns columns.)
QString codeFontFamily(const QFont &base)
{
    if (QFontInfo(base).fixedPitch())
        return base.family();
    static const QString preferred = [] {
        const QStringList installed = QFontDatabase::families();
        for (const char *candidate : {"Cascadia Mono", "Cascadia Code", "Consolas",
                                      "JetBrains Mono", "SF Mono", "Menlo",
                                      "DejaVu Sans Mono", "Liberation Mono"}) {
            if (installed.contains(QLatin1String(candidate)))
                return QString::fromLatin1(candidate);
        }
        return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    }();
    return preferred;
}

// Code surface geometry (device-independent px), shared by the block margins
// styleCodeInDocument() sets and the rounded rect paintCodeSurfaces() draws.
constexpr int kCodeGapPx = 8;       // air between the surface and neighbouring prose
constexpr int kCodePadV = 6;        // text -> surface edge, top and bottom
constexpr int kCodePadH = 10;       // text -> surface edge, left and right
constexpr double kCodeRadius = 6.0; // corner radius

// A slice of the text colour mixed into the base colour: always a visible step
// lighter on dark themes and darker on light ones, whatever the palette.
QColor mixTextIntoBase(const QPalette &pal, int textPct)
{
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    auto mix = [textPct](int b, int t) { return (b * (100 - textPct) + t * textPct) / 100; };
    return QColor(mix(base.red(), text.red()),
                  mix(base.green(), text.green()),
                  mix(base.blue(), text.blue()));
}

QColor codeBorderColor(const QPalette &pal)
{
    return mixTextIntoBase(pal, 28);
}

// Code surface fill: the bubble is palette(base) and AlternateBase is nearly
// identical to it on most themes, so code blocks vanished into the bubble.
QColor codeSurfaceColor(const QPalette &pal)
{
    return mixTextIntoBase(pal, 14);
}

// A block whose only content is one inline image (display math).
bool isImageOnlyBlock(const QTextBlock &b)
{
    int fragments = 0;
    bool image = false;
    for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid()) continue;
        ++fragments;
        image = f.charFormat().isImageFormat();
    }
    return fragments == 1 && image;
}

// After setHtml: hand each rendered formula to the document as the image
// resource its "math:<n>" <img> names (setHtml clears resources, so this must
// run after it and before layout), pin the image's device-independent size, and
// place it — inline images middle-aligned (they are padded symmetric about the
// math axis), display images centered in their own paragraph.
void installMath(QTextDocument *doc, const MarkdownPreprocessor::Result &pre,
                 double inlinePx, double displayPx, const QColor &color, double dpr)
{
    if (!doc || pre.math.isEmpty()) return;
    const int n = int(pre.math.size());
    QVector<QSizeF> sizes(n);
    for (int i = 0; i < n; ++i) {
        const MarkdownPreprocessor::MathSpan &m = pre.math.at(i);
        const auto r = Math::RenderCache::instance().get(
            m.source, m.display, m.display ? displayPx : inlinePx, color, dpr);
        if (!r) continue; // accepted earlier, so a hit; stays an empty <img> if evicted+failed
        doc->addResource(QTextDocument::ImageResource,
                         QUrl(QStringLiteral("math:%1").arg(i)), r->image);
        sizes[i] = QSizeF(r->image.width() / dpr, r->image.height() / dpr);
    }

    struct Fix { int pos; int len; QTextImageFormat fmt; };
    QVector<Fix> fixes;
    QVector<int> displayBlocks;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        bool anyDisplay = false;
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid() || !f.charFormat().isImageFormat()) continue;
            QTextImageFormat fmt = f.charFormat().toImageFormat();
            if (!fmt.name().startsWith(QLatin1String("math:"))) continue;
            bool ok = false;
            const int idx = fmt.name().mid(5).toInt(&ok);
            if (!ok || idx < 0 || idx >= n || sizes[idx].isEmpty()) continue;
            fmt.setWidth(sizes[idx].width());
            fmt.setHeight(sizes[idx].height());
            fmt.setVerticalAlignment(QTextCharFormat::AlignMiddle);
            fixes.append({f.position(), f.length(), fmt});
            anyDisplay = anyDisplay || pre.math.at(idx).display;
        }
        if (anyDisplay && isImageOnlyBlock(b))
            displayBlocks.append(b.position());
    }

    QTextCursor cur(doc);
    cur.beginEditBlock();
    for (const Fix &fx : fixes) {
        QTextCursor fc(doc);
        fc.setPosition(fx.pos);
        fc.setPosition(fx.pos + fx.len, QTextCursor::KeepAnchor);
        fc.setCharFormat(fx.fmt);
    }
    for (int pos : displayBlocks) {
        QTextCursor bc(doc->findBlock(pos));
        QTextBlockFormat bf = bc.blockFormat();
        bf.setAlignment(Qt::AlignHCenter);
        bc.setBlockFormat(bf);
    }
    cur.endEditBlock();
}

// Document mode (the file preview): restore rhythm between blocks that the flat
// chat layout deliberately drops. One O(blocks) walk.
//   - paragraph: `gap` below
//   - heading: extra space above, half a gap below (it belongs to what follows)
//   - list item: tight to the next item, a full gap after the list ends
//   - untouched: code surfaces (they own their gap + padding), table cells (cell
//     padding does the work), display math, and the fence separator paragraph
//   - first block's top and last block's bottom stay 0 (no phantom edge space)
void applyDocumentSpacing(QTextDocument *doc, int gap)
{
    if (!doc) return;
    const int itemGap = qMax(1, gap / 4);
    const QTextBlock first = doc->begin();
    const QTextBlock last = doc->lastBlock();
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
        const QTextBlockFormat bf = b.blockFormat();
        if (bf.background().style() != Qt::NoBrush) continue;
        QTextCursor probe(b);
        if (probe.currentTable()) continue;
        if (isImageOnlyBlock(b)) continue;
        if (b.text() == QString(QChar(0x200B))) continue;

        int top = 0;
        int bottom = gap;
        if (bf.headingLevel() > 0) {
            top = bf.headingLevel() <= 2 ? gap + gap / 2 : gap;
            bottom = qMax(1, gap / 2);
        } else if (b.textList()) {
            const QTextBlock next = b.next();
            bottom = next.isValid() && next.textList() ? itemGap : gap;
        }
        if (b == first) top = 0;
        if (b == last) bottom = 0;

        QTextBlockFormat fmt;
        fmt.setTopMargin(top);
        fmt.setBottomMargin(bottom);
        probe.mergeBlockFormat(fmt);
    }
}

// <details> toggle rows are links to the private scheme; draw them as a quiet
// disclosure row (text colour, semi-bold, no underline) rather than a blue link.
void styleDetailsRows(QTextDocument *doc, const QColor &textColor)
{
    if (!doc) return;
    struct Fix { int pos; int len; };
    QVector<Fix> fixes;
    const QString prefix = QString::fromLatin1(MarkdownPreprocessor::kDetailsScheme);
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.isValid() && f.charFormat().isAnchor()
                && f.charFormat().anchorHref().startsWith(prefix))
                fixes.append({f.position(), f.length()});
        }
    }
    if (fixes.isEmpty()) return;
    QTextCharFormat style;
    style.setFontUnderline(false);
    style.setForeground(textColor);
    style.setFontWeight(QFont::DemiBold);
    QTextCursor cur(doc);
    cur.beginEditBlock();
    for (const Fix &fx : fixes) {
        QTextCursor fc(doc);
        fc.setPosition(fx.pos);
        fc.setPosition(fx.pos + fx.len, QTextCursor::KeepAnchor);
        fc.mergeCharFormat(style);
    }
    cur.endEditBlock();
}

// <div align>/<p align>/<center>: the preprocessor wrapped the content in a
// pair of sentinel paragraphs. Apply the alignment to the blocks between them
// and delete the sentinels.
void applyAlignmentSentinels(QTextDocument *doc)
{
    if (!doc) return;
    const QChar begin = MarkdownPreprocessor::kAlignBegin;
    const QChar end = MarkdownPreprocessor::kAlignEnd;
    QVector<int> sentinelPositions;
    Qt::Alignment current;
    bool active = false;

    QTextCursor edit(doc);
    edit.beginEditBlock();
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const QString t = b.text();
        if (t.size() == 2 && t.at(0) == begin) {
            switch (t.at(1).unicode()) {
            case 'l': current = Qt::AlignLeft; break;
            case 'r': current = Qt::AlignRight; break;
            case 'j': current = Qt::AlignJustify; break;
            default: current = Qt::AlignHCenter; break;
            }
            active = true;
            sentinelPositions.append(b.position());
            continue;
        }
        if (t.size() == 1 && t.at(0) == end) {
            active = false;
            sentinelPositions.append(b.position());
            continue;
        }
        if (active) {
            QTextCursor bc(b);
            QTextBlockFormat bf = bc.blockFormat();
            bf.setAlignment(current);
            bc.setBlockFormat(bf);
        }
    }
    // Delete back to front so earlier positions stay valid. A sentinel block is
    // removed together with the paragraph separator that precedes it (or, for
    // the very first block, the one that follows).
    for (int i = int(sentinelPositions.size()) - 1; i >= 0; --i) {
        const QTextBlock b = doc->findBlock(sentinelPositions.at(i));
        if (!b.isValid()) continue;
        QTextCursor rc(doc);
        const QTextBlock prev = b.previous();
        if (prev.isValid()) {
            rc.setPosition(prev.position() + prev.length() - 1);
            rc.setPosition(b.position() + b.length() - 1, QTextCursor::KeepAnchor);
        } else {
            rc.setPosition(b.position());
            const QTextBlock next = b.next();
            rc.setPosition(next.isValid() ? next.position() : b.position() + b.length() - 1,
                           QTextCursor::KeepAnchor);
        }
        rc.removeSelectedText();
    }
    edit.endEditBlock();
}
// Agent output uses bare \n for visual line breaks, but CommonMark treats a
// single newline as a soft break (rendered as a space). Convert lone \n into
// hard breaks (two trailing spaces before the newline) so they render visually.
// Preserves code fences, blank-line paragraph separators, and lines that
// already end with trailing spaces or a backslash hard break.
QString ensureHardBreaks(const QString &md)
{
    const QStringList lines = md.split(QLatin1Char('\n'));
    QString out;
    out.reserve(md.size() + lines.size() * 2);
    bool inFence = false;

    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines[i];

        if (line.startsWith(QLatin1String("```")) || line.startsWith(QLatin1String("~~~"))) {
            inFence = !inFence;
        }

        out += line;

        if (i < lines.size() - 1) {
            const bool nextIsBlank = (i + 1 < lines.size()) && lines[i + 1].trimmed().isEmpty();
            const bool alreadyHard = line.endsWith(QLatin1String("  "))
                                     || line.endsWith(QLatin1Char('\\'));
            if (!inFence && !nextIsBlank && !line.trimmed().isEmpty() && !alreadyHard) {
                out += QLatin1String("  ");
            }
            out += QLatin1Char('\n');
        }
    }
    return out;
}

// Qt lays a block's top margin out only after another block and never adds the
// last block's bottom margin, so a document that starts or ends with a code
// block has no room for that surface's padding: owners fitting their height to
// the document clipped it. The root frame's margins do count: reserve the
// padding (and the border) there, never less than the document margin.
void reserveEdgeCodePadding(QTextDocument *doc)
{
    auto isCode = [](const QTextBlock &b) {
        return b.isValid() && b.blockFormat().background().style() != Qt::NoBrush
               && !QTextCursor(b).currentTable();
    };
    const bool first = isCode(doc->firstBlock());
    const bool last = isCode(doc->lastBlock());
    if (!first && !last) return;
    const qreal reserve = qMax(doc->documentMargin(), qreal(kCodePadV + 1));
    QTextFrameFormat ff = doc->rootFrame()->frameFormat();
    if (first) ff.setTopMargin(reserve);
    if (last) ff.setBottomMargin(reserve);
    doc->rootFrame()->setFrameFormat(ff);
}

} // namespace

MarkdownView::MarkdownView(QWidget *parent)
    : QTextBrowser(parent)
{
    // Links are routed by onAnchorClicked: http/https/mailto go to the OS, the
    // private <details> scheme toggles a section, everything else is ignored
    // (QTextBrowser would otherwise try to navigate to it).
    setOpenLinks(false);
    connect(this, &QTextBrowser::anchorClicked, this, &MarkdownView::onAnchorClicked);
    // Hover/move/leave on the viewport drive the copy-code button; the filter has to
    // be installed up front so the very first mouse move is seen.
    viewport()->setMouseTracking(true);
    viewport()->installEventFilter(this);
    ensureCopyButton();
    // A vertical scrollbar appearing/disappearing changes the room left for the
    // column: keep it centred.
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this]() {
        if (m_maxContentWidth > 0)
            applyViewportMargins();
    });
}

void MarkdownView::setChatFont(const QFont &font, bool rerenderNow)
{
    m_chatFont = font;
    m_chatFontSet = true;
    // QTextDocument::setDefaultFont is the authoritative knob for body text; the
    // render also re-bakes the font into HTML (toHtml carries the default font).
    document()->setDefaultFont(font);
    if (rerenderNow && !m_source.isEmpty())
        rerender();
}

void MarkdownView::setMarkdownSource(const QString &text)
{
    m_source = text;
    rerender();
}

void MarkdownView::setDocumentSpacing(bool on)
{
    if (m_documentSpacing == on) return;
    m_documentSpacing = on;
    if (!m_source.isEmpty())
        rerender();
}

void MarkdownView::setMaxContentWidth(int px)
{
    m_maxContentWidth = qMax(0, px);
    // The margins either side of a capped column must look like the document, not
    // like the window: paint them with the base colour. (Uncapped views — the chat
    // bubbles — are transparent through their stylesheet and keep it that way.)
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(m_maxContentWidth > 0);
    applyViewportMargins();
}

void MarkdownView::applyViewportMargins()
{
    if (m_inMarginUpdate) return;
    int side = 0;
    if (m_maxContentWidth > 0) {
        const int scrollbar = verticalScrollBar() && verticalScrollBar()->isVisible()
                                  ? verticalScrollBar()->width() : 0;
        const int available = width() - 2 * frameWidth() - scrollbar;
        side = qMax(0, (available - m_maxContentWidth) / 2);
    }
    if (side == m_appliedSide) return;
    m_inMarginUpdate = true;
    m_appliedSide = side;
    // setViewportMargins is the QAbstractScrollArea knob: it insets the viewport
    // (and so the text column) without touching the scrollbars.
    setViewportMargins(side, 0, side, 0);
    // QTextEdit only re-wraps its document on a widget resize, not when the
    // viewport changes size by itself: replay one so the text follows the column.
    QResizeEvent relayout(size(), size());
    QTextBrowser::resizeEvent(&relayout);
    m_inMarginUpdate = false;
}

void MarkdownView::resizeEvent(QResizeEvent *event)
{
    if (m_maxContentWidth > 0)
        applyViewportMargins(); // before the base class lays the document out
    QTextBrowser::resizeEvent(event);
    // The hovered code region's geometry moved with the reflow; re-place (or
    // hide) the copy button against the new layout.
    if (m_copyCodeBtn && m_copyCodeBtn->isVisible())
        updateCopyButtonForPos(viewport()->mapFromGlobal(QCursor::pos()));
}

void MarkdownView::paintEvent(QPaintEvent *event)
{
    // Code surfaces sit behind the text: draw them first, then let QTextBrowser
    // paint the document over them.
    if (!m_codeRegions.isEmpty()) {
        QPainter p(viewport());
        p.setClipRect(event->rect());
        paintCodeSurfaces(p);
    }
    QTextBrowser::paintEvent(event);
}

void MarkdownView::changeEvent(QEvent *event)
{
    QTextBrowser::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        // Re-tint the copy glyph and re-skin code/math for the new theme.
        rebuildCopyIcon();
        if (!m_source.isEmpty())
            rerender();
    }
}

void MarkdownView::normalizeBlockMargins(QTextDocument *doc)
{
    normalizeBlockMarginsImpl(doc);
}

void MarkdownView::onAnchorClicked(const QUrl &url)
{
    const QString s = url.toString();
    const QLatin1String detailsScheme(MarkdownPreprocessor::kDetailsScheme);
    if (s.startsWith(detailsScheme)) {
        bool ok = false;
        const int ordinal = s.mid(detailsScheme.size()).toInt(&ok);
        if (!ok || ordinal < 0) return;
        if (!m_detailsFlipped.remove(ordinal))
            m_detailsFlipped.insert(ordinal);
        rerender(); // immediate: a click should not wait on any owner debounce
        return;
    }
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https")
        || scheme == QLatin1String("mailto"))
        QDesktopServices::openUrl(url);
}

void MarkdownView::rerender()
{
    const QString borderColor = palette().color(QPalette::Mid).name();
    const QString headerBg = palette().color(QPalette::AlternateBase).name();
    document()->setDefaultStyleSheet(QStringLiteral(
        "table { border-collapse: collapse; }"
        "th, td { border: 1px solid %1; padding: 8px; }"
        "th { background-color: %2; }"
    ).arg(borderColor, headerBg) + codeStyleSheet());

    // QTextDocument renders markdown tables with default cellspacing,
    // producing visible double borders. Re-emit the HTML with
    // cellspacing="0" so border-collapse actually collapses.
    QTextDocument tmp;
    // toHtml() bakes the document's default font (family + size) into the
    // serialized <body> as absolute styles, which then win over anything the
    // target document's defaultFont would supply. A default-constructed
    // QTextDocument uses QApplication::font() (~Segoe UI 9pt on Windows), so
    // without seeding it the body renders in the app font instead of the chat
    // typeface. Seed with the chat font so the round-trip carries it for prose;
    // code keeps Qt's monospace family here and is rewritten in
    // styleCodeInDocument() below.
    if (m_chatFontSet) tmp.setDefaultFont(m_chatFont);
    // MarkdownNoHTML: agent output is markdown, never trusted HTML. Qt's
    // default MarkdownDialectGitHub leaves md4c's HTML parsing on, so a bare
    // angle-bracket construct (Surreal<Db>, vector<int>, "a < b") is consumed
    // as an inline HTML tag — it swallows everything after it and only
    // inline-code fragments survive, garbling the document. Math delimiters and
    // the small allowlisted HTML subset (<details>, align, inline tags) are
    // rewritten into plain markdown first; anything unrecognised passes through
    // untouched, so NoHTML below keeps holding.
    const QFont mathBase = m_chatFontSet ? m_chatFont : font();
    // STIX glyphs have a smaller x-height than typical UI fonts, so scale up
    // a little to sit level with the surrounding text.
    const double inlineMathPx = qMax(8, QFontInfo(mathBase).pixelSize()) * 1.1;
    const double displayMathPx = inlineMathPx * 1.2;
    const QColor mathColor = palette().color(QPalette::Text);
    const double dpr = devicePixelRatioF();
    const MarkdownPreprocessor::Result pre = MarkdownPreprocessor::run(
        m_source, m_detailsFlipped,
        [&](const QString &src, bool displayStyle) {
            // Also warms the cache: installMath() below is then all hits.
            return Math::RenderCache::instance()
                .get(src, displayStyle, displayStyle ? displayMathPx : inlineMathPx,
                     mathColor, dpr)
                .has_value();
        });
    tmp.setMarkdown(ensureHardBreaks(pre.markdown),
                    QTextDocument::MarkdownFeatures(
                        QTextDocument::MarkdownDialectGitHub
                        | QTextDocument::MarkdownNoHTML));
    markCodeBlocks(tmp);
    QString html = tmp.toHtml();
    markCodeFamilies(html, tmp.defaultFont().family());
    html.replace(QRegularExpression(QStringLiteral("<table([^>]*)>")),
                 QStringLiteral("<table\\1 cellspacing=\"0\" cellpadding=\"8\">"));
    document()->setHtml(html);
    installMath(document(), pre, inlineMathPx, displayMathPx, mathColor, dpr);
    // Skin code on the document model (reliable, unlike QTextDocument's CSS
    // subset and unlike fragile HTML string surgery): detect code blocks /
    // inline code by their marker family, then apply the block marker, rewrite
    // the code family, and add vertical breathing. Must run BEFORE
    // normalizeBlockMargins so the code-region margins it sets survive (that
    // pass skips background-bearing blocks).
    styleCodeInDocument();
    if (pre.hasAlign)
        applyAlignmentSentinels(document());
    if (pre.hasDetails)
        styleDetailsRows(document(), mathColor);
    normalizeBlockMarginsImpl(document());
    if (m_documentSpacing)
        applyDocumentSpacing(document(), QFontMetrics(document()->defaultFont()).height() / 2);
    reserveEdgeCodePadding(document());
    // Map fenced-code regions so the hover copy button can target them.
    scanCodeRegions();
    emit rendered();
}

QString MarkdownView::codeStyleSheet() const
{
    // Inline <code> only. Its background is a CHAR-format fill: QTextHtmlParser
    // promotes a CSS background to the block format only for <hr> — for every
    // other element (including <pre>) the CSS background lands on the char
    // format, painting behind glyphs at text width. Inline code has no
    // block-level fill path, so this rule is the only thing that tints it.
    //
    // Deliberately NO `pre` rule here. A <pre> CSS background would also be a
    // char-level fill (text width, and absent on blank lines because they have
    // no fragment) — it would NOT set the block's background brush. The
    // full-width block fill, border-equivalent, and the blank-line handling all
    // live in styleCodeInDocument() on the document model, which is the single
    // source of truth for fenced-block chrome. Adding a `pre` background here
    // would layer a second, partial fill over the model fill and decouple the
    // visual from the brush that scanCodeRegions()/normalizeBlockMargins() read.
    //
    // No padding on `code` by toolkit constraint: QTextDocument applies CSS
    // `padding` only to table cells (qtextdocumentfragment: isTableCell guard) —
    // inline runs have no box model, so inline padding is silently dropped and
    // would be dead code. Inline code therefore keeps a tight tint; faking inset
    // with thin spaces would pollute selection/copy. Fenced-block breathing
    // (line height + region margins) is handled on the model in styleCodeInDocument().
    const QString fill = codeSurfaceColor(palette()).name();
    return QStringLiteral("code { background-color: %1; }").arg(fill);
}

void MarkdownView::styleCodeInDocument()
{
    QTextDocument *doc = document();
    if (!doc) return;

    // Family to apply to code so it matches the chat text. setMarkdown tags
    // code with QFontDatabase's system fixed font; toHtml() bakes that literal
    // family inline (e.g. 'Consolas'/'Courier New') whenever it differs from the
    // seeded default, so without this rewrite code renders in the system fixed
    // font instead of the chat font. Done on the model (not regex on the HTML):
    // robust to Qt's serialization and idempotent — rerender() rebuilds from raw
    // markdown each time. Fall back to the document default family when no chat
    // font has been pushed yet.
    const QString codeFamily = codeFontFamily(m_chatFontSet ? m_chatFont : doc->defaultFont());

    // The visible surface (rounded, padded, bordered) is painted by
    // paintCodeSurfaces(). The block background set below is only a transparent
    // MARKER that scanCodeRegions()/normalizeBlockMargins() key off; Qt's own
    // block fill would stripe per line and cannot round or pad. What the model
    // contributes is geometry: left/right margins pad the text inside the surface,
    // first/last block margins reserve the surface's vertical padding plus the gap
    // to neighbouring prose, and lines stay at single height so they read as one
    // block.
    const QBrush surfaceMarker(Qt::transparent);
    const QBrush inlineCodeFill(codeSurfaceColor(palette()));

    // Classification, verified empirically against THIS Qt build by running the
    // real setMarkdown -> toHtml -> setHtml pipeline and inspecting the result:
    //
    //  - nonBreakableLines() does NOT survive the round-trip. toHtml() emits each
    //    code line as `<pre style="...margin...">` with NO `whitespace` decl, so
    //    re-import never calls setNonBreakableLines — every block reads nbl=0.
    //    BlockCodeFence and fontFixedPitch are likewise dropped. Keying off any of
    //    them finds zero code (kills BOTH the fill/margins and the copy button).
    //
    //  - What DOES survive: each code fragment's font family is the literal string
    //    "monospace" (the exporter writes `<span style="font-family:'monospace'">`
    //    for inline AND fenced code; re-import keeps it). So detect code by a
    //    monospace family on the fragment. This covers inline `<code>` and every
    //    non-blank fenced line. Which of the two it is comes from the block
    //    marker family markCodeBlocks() sets before the round trip.
    //
    //  - A blank line inside a fence is an EMPTY block (no fragment, so no family
    //    signal). Promote an empty block to code when code flanks it on both
    //    sides, else the fill gaps and the copy region splits.
    auto fragIsCode = [](const QTextCharFormat &cf) {
        const QStringList fams = cf.fontFamilies().toStringList();
        for (const QString &f : fams) {
            if (f == QLatin1String(kCodeFamilyMarker)
                || f.contains(QLatin1String("mono"), Qt::CaseInsensitive))
                return true;
        }
        return cf.fontFixedPitch(); // belt-and-suspenders for other Qt paths
    };

    auto fragIsBlockCode = [](const QTextCharFormat &cf) {
        return cf.fontFamilies().toStringList().contains(QLatin1String(kCodeBlockFamilyMarker));
    };

    // First pass: per-block kind (Prose / Code / Empty) + collect code fragment
    // ranges for the family rewrite. A block is Code if markCodeBlocks() marked
    // it (a fenced or indented code block); Empty if it has no fragments at all
    // (blank fence line). Every other code fragment is an inline span, even when
    // it fills its paragraph or list item on its own.
    enum class Kind : std::uint8_t { Prose, Code, Empty };
    QVector<Kind> kinds;
    QVector<int> positions;
    QVector<QPair<int, int>> codeFragRanges;   // char ranges to re-family
    QVector<QPair<int, int>> inlineCodeRanges; // inline spans
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        bool blockCode = false, hasAny = false;
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid()) continue;
            hasAny = true;
            const QTextCharFormat cf = frag.charFormat();
            if (fragIsBlockCode(cf)) {
                blockCode = true;
                codeFragRanges.append({frag.position(), frag.position() + frag.length()});
            } else if (fragIsCode(cf)) {
                // An inline span gets a char-level fill. (Qt exports inline code
                // as a <span>, not <code>, so a CSS `code` rule never matches.)
                codeFragRanges.append({frag.position(), frag.position() + frag.length()});
                inlineCodeRanges.append(codeFragRanges.constLast());
            }
        }
        kinds.append(blockCode ? Kind::Code : !hasAny ? Kind::Empty : Kind::Prose);
        positions.append(b.position());
    }

    // Second pass: promote an Empty block enclosed by code (scanning over runs of
    // Empties) so an internal blank line keeps its fill and the region stays one
    // contiguous run with one copy button.
    auto codeOnSide = [&](int from, int step) {
        for (int i = from; i >= 0 && i < kinds.size(); i += step) {
            if (kinds[i] == Kind::Empty) continue;
            return kinds[i] == Kind::Code;
        }
        return false;
    };
    QVector<int> codeBlockPositions; // first position of each code block
    for (int i = 0; i < kinds.size(); ++i) {
        if (kinds[i] == Kind::Empty && codeOnSide(i - 1, -1) && codeOnSide(i + 1, 1))
            kinds[i] = Kind::Code;
        if (kinds[i] == Kind::Code)
            codeBlockPositions.append(positions[i]);
    }

    QTextCursor cur(doc);
    cur.beginEditBlock();

    // Block chrome: background + line height on each fenced code block; region
    // top/bottom margins on the first/last block of each contiguous run so the
    // region separates from adjacent prose without doubling the gap internally.
    auto isCodeBlockAt = [&](int pos) {
        return pos >= 0 && codeBlockPositions.contains(pos);
    };
    for (int pos : codeBlockPositions) {
        const QTextBlock b = doc->findBlock(pos);
        if (!b.isValid()) continue;
        const QTextBlock prev = b.previous();
        const QTextBlock next = b.next();
        const bool prevIsCode = prev.isValid() && isCodeBlockAt(prev.position());
        const bool nextIsCode = next.isValid() && isCodeBlockAt(next.position());

        QTextBlockFormat bf = b.blockFormat();
        bf.setBackground(surfaceMarker);
        bf.setLeftMargin(kCodePadH);
        bf.setRightMargin(kCodePadH);
        bf.setTopMargin(prevIsCode ? 0 : kCodeGapPx + kCodePadV);
        bf.setBottomMargin(nextIsCode ? 0 : kCodeGapPx + kCodePadV);
        QTextCursor bc(b);
        bc.setBlockFormat(bf);
    }

    // Family rewrite for every code fragment (fenced + inline), so code follows
    // the chat font instead of the baked monospace family.
    QTextCharFormat repl;
    repl.setFontFamilies({codeFamily});
    repl.setFontFixedPitch(false); // family is authoritative now
    for (const auto &range : codeFragRanges) {
        QTextCursor fc(doc);
        fc.setPosition(range.first);
        fc.setPosition(range.second, QTextCursor::KeepAnchor);
        fc.mergeCharFormat(repl);
    }
    QTextCharFormat inlineFill;
    inlineFill.setBackground(inlineCodeFill);
    for (const auto &range : inlineCodeRanges) {
        QTextCursor fc(doc);
        fc.setPosition(range.first);
        fc.setPosition(range.second, QTextCursor::KeepAnchor);
        fc.mergeCharFormat(inlineFill);
    }
    cur.endEditBlock();
}

void MarkdownView::ensureCopyButton()
{
    if (m_copyCodeBtn) return;
    // Parent to the viewport so the button floats over the scrolled content and
    // is clipped to the visible body, not the frame chrome.
    m_copyCodeBtn = new QToolButton(viewport());
    m_copyCodeBtn->setCursor(Qt::PointingHandCursor);
    m_copyCodeBtn->setFocusPolicy(Qt::NoFocus);
    m_copyCodeBtn->setToolTip(tr("Copy code"));
    m_copyCodeBtn->setAutoRaise(true);
    m_copyCodeBtn->setIconSize(QSize(16, 16));
    // Semi-transparent pill so the glyph reads over any code-surface tint,
    // following the media-overlay chrome convention.
    m_copyCodeBtn->setStyleSheet(QStringLiteral(
        "QToolButton { background: rgba(128,128,128,0.22); border: none; "
        "border-radius: 4px; padding: 2px; }"
        "QToolButton:hover { background: rgba(128,128,128,0.40); }"));
    m_copyCodeBtn->hide();
    rebuildCopyIcon();
    connect(m_copyCodeBtn, &QToolButton::clicked, this, [this]() {
        if (m_hoverCodeIndex < 0 || m_hoverCodeIndex >= m_codeRegions.size())
            return;
        QApplication::clipboard()->setText(m_codeRegions[m_hoverCodeIndex].text);
    });
    // Watch the viewport for hover/move/leave to drive button visibility.
    viewport()->setMouseTracking(true);
    viewport()->installEventFilter(this);
}

void MarkdownView::rebuildCopyIcon()
{
    if (!m_copyCodeBtn) return;
    m_copyCodeBtn->setIcon(tintedCodeIcon(QStringLiteral(":/icons/copy.svg"),
                                          palette().color(QPalette::WindowText)));
}

void MarkdownView::scanCodeRegions()
{
    m_codeRegions.clear();
    m_hoverCodeIndex = -1;
    if (m_copyCodeBtn) m_copyCodeBtn->hide();

    QTextDocument *doc = document();
    // Fenced code blocks carry the AlternateBase background our pre rule sets;
    // prose blocks have no block background. Group consecutive code blocks (a
    // multi-line fence is several blocks) into one region. Table cells can also
    // carry a background, so exclude any block that lives inside a table.
    bool inRegion = false;
    CodeRegion cur{};
    QStringList curLines;
    auto flush = [&]() {
        if (!inRegion) return;
        cur.text = curLines.join(QLatin1Char('\n'));
        m_codeRegions.append(cur);
        inRegion = false;
        curLines.clear();
    };
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        QTextCursor probe(b);
        const bool inTable = probe.currentTable() != nullptr;
        const bool isCode = !inTable
                            && b.blockFormat().background().style() != Qt::NoBrush;
        if (isCode) {
            if (!inRegion) {
                inRegion = true;
                cur = CodeRegion{};
                cur.start = b.position();
                curLines.clear();
            }
            cur.end = b.position() + b.length() - 1;
            curLines.append(b.text());
        } else {
            flush();
        }
    }
    flush();
    viewport()->update();
}

void MarkdownView::paintCodeSurfaces(QPainter &p)
{
    if (m_codeRegions.isEmpty()) return;
    QTextDocument *doc = document();
    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    if (!layout) return;
    const QPalette pal = palette();
    const double scrollY = verticalScrollBar()->value();
    // The surface spans the text column: an owner may inset the document
    // through the root frame (a tool card's result lines up with its title).
    const QTextFrameFormat frame = doc->rootFrame()->frameFormat();
    const double left = frame.leftMargin();
    const double width = viewport()->width() - left - frame.rightMargin();

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(codeBorderColor(pal), 1.0));
    p.setBrush(codeSurfaceColor(pal));
    for (const CodeRegion &r : std::as_const(m_codeRegions)) {
        const QTextBlock first = doc->findBlock(r.start);
        const QTextBlock last = doc->findBlock(r.end);
        if (!first.isValid() || !last.isValid()) continue;
        // blockBoundingRect excludes the block margins; the margins styleCode…
        // set are exactly the gap + padding, so pad outwards from the text rows.
        const QRectF top = layout->blockBoundingRect(first);
        const QRectF bottom = layout->blockBoundingRect(last);
        const QRectF rect(left + 0.5, top.top() - kCodePadV - scrollY + 0.5, width - 1.0,
                          bottom.bottom() - top.top() + 2.0 * kCodePadV - 1.0);
        p.drawRoundedRect(rect, kCodeRadius, kCodeRadius);
    }
    p.restore();
}

void MarkdownView::updateCopyButtonForPos(const QPoint &viewportPos)
{
    if (!m_copyCodeBtn) return;
    if (m_codeRegions.isEmpty()) {
        m_hoverCodeIndex = -1;
        m_copyCodeBtn->hide();
        return;
    }
    QTextDocument *doc = document();
    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    if (!layout) return;
    // Map the viewport point into document coordinates (account for scroll).
    const QPoint docPos = viewportPos
        + QPoint(horizontalScrollBar()->value(),
                 verticalScrollBar()->value());
    const int hit = layout->hitTest(docPos, Qt::FuzzyHit);

    int idx = -1;
    for (int i = 0; i < m_codeRegions.size(); ++i) {
        if (hit >= m_codeRegions[i].start && hit <= m_codeRegions[i].end) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        m_hoverCodeIndex = -1;
        m_copyCodeBtn->hide();
        return;
    }

    m_hoverCodeIndex = idx;
    // Anchor to the top-right of the region's first block. Use the viewport's
    // right edge (not the block's text width) so a short code line still gets
    // the button in the corner where users expect it.
    QTextBlock first = doc->findBlock(m_codeRegions[idx].start);
    const QRectF blockRect = layout->blockBoundingRect(first);
    const int vpScrollY = verticalScrollBar()->value();
    const int vpW = viewport()->width();
    const int btnW = m_copyCodeBtn->sizeHint().width();
    const int btnH = m_copyCodeBtn->sizeHint().height();
    const int x = qMax(4, vpW - btnW - 6);
    const int y = static_cast<int>(blockRect.top()) - kCodePadV - vpScrollY + 4;
    m_copyCodeBtn->resize(btnW, btnH);
    m_copyCodeBtn->move(x, y);
    m_copyCodeBtn->raise();
    m_copyCodeBtn->show();
}

bool MarkdownView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == viewport()) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(event);
            ensureCopyButton();
            updateCopyButtonForPos(me->position().toPoint());
            break;
        }
        case QEvent::Leave:
            // Don't hide while the pointer is over the button itself (the button
            // is a child of the viewport, so moving onto it fires Leave here).
            if (m_copyCodeBtn && !m_copyCodeBtn->underMouse()) {
                m_hoverCodeIndex = -1;
                m_copyCodeBtn->hide();
            }
            break;
        default:
            break;
        }
    }
    return QTextBrowser::eventFilter(watched, event);
}
