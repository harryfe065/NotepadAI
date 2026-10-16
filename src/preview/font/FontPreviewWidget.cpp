#include "FontPreviewWidget.h"

#include "FontSampleView.h"
#include "GlyphGridView.h"
#include "ImageDecoder.h"
#include "UnicodeBlocks.h"

#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QLocale>
#include <QScrollArea>
#include <QSplitter>
#include <QtConcurrent>

#include <cstdint>
#include <algorithm>

namespace {

constexpr QStringView kPangram = u"The quick brown fox jumps over the lazy dog 0123456789";

// Grid sources in the combo: all characters, one block, characters in no
// listed block, or every glyph by id.
enum SourceKind : std::int8_t { AllCharacters = -1, OtherCharacters = -2, AllGlyphs = -3 };

using CmapIt = std::vector<std::pair<char32_t, quint32>>::const_iterator;

std::pair<CmapIt, CmapIt> blockRange(const std::vector<std::pair<char32_t, quint32>> &cmap,
                                     const UnicodeBlocks::Block &block)
{
    const auto lo = std::lower_bound(cmap.begin(), cmap.end(), char32_t(block.first),
                                     [](const auto &e, char32_t cp) { return e.first < cp; });
    const auto hi = std::upper_bound(lo, cmap.end(), char32_t(block.last),
                                     [](char32_t cp, const auto &e) { return cp < e.first; });
    return {lo, hi};
}

QString truncated(const QString &s, int max)
{
    return s.size() > max ? s.left(max) + QChar(0x2026) : s;
}

} // namespace

FontPreviewWidget::FontPreviewWidget(const QString &typeId, const QString &displayName, QWidget *parent)
    : AssetPreviewWidget(typeId, displayName, parent)
{
    m_faces = new QComboBox;
    m_faces->setToolTip(tr("Face in this collection"));
    m_faces->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_faces->setMaximumWidth(260);
    m_faces->hide();
    addToolWidget(m_faces);

    m_sample = new QLineEdit;
    m_sample->setPlaceholderText(tr("Sample text"));
    m_sample->setToolTip(tr("Sample text"));
    m_sample->setClearButtonEnabled(true);
    m_sample->setMinimumWidth(240);
    addToolWidget(m_sample);
    addToolGap();

    m_source = new QComboBox;
    m_source->setToolTip(tr("Characters shown in the grid"));
    m_source->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_source->setMaximumWidth(320);
    addToolWidget(m_source);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);
    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    m_sampleView = new FontSampleView;
    scroll->setWidget(m_sampleView);
    splitter->addWidget(scroll);
    m_grid = new GlyphGridView;
    splitter->addWidget(m_grid);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    setContentWidget(splitter);

    connect(m_faces, QOverload<int>::of(&QComboBox::activated), this, &FontPreviewWidget::selectFace);
    connect(m_sample, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_sampleView->setText(text.isEmpty() ? defaultSample() : text);
    });
    connect(m_source, QOverload<int>::of(&QComboBox::activated), this, &FontPreviewWidget::applySource);
    connect(m_grid, &GlyphGridView::hovered, this, &FontPreviewWidget::showHover);
    connect(&m_watcher, &QFutureWatcher<Loaded>::finished, this, &FontPreviewWidget::onLoaded);
    connect(&m_faceWatcher, &QFutureWatcher<FaceBuild>::finished, this, [this]() {
        const FaceBuild built = m_faceWatcher.result();
        if (built.serial == m_loadSerial)
            applyFace(built.face, built.bytes);
    });
}

void FontPreviewWidget::startLoad(bool reload)
{
    m_pendingReload = reload;
    const QString path = filePath();
    const QFileInfo info(path);
    if (!info.exists()) {
        showBanner(BannerKind::Error, tr("The file no longer exists."));
        return;
    }
    m_fileSize = info.size();
    if (m_fileSize > FontFile::kMaxFileBytes) {
        showBanner(BannerKind::Error,
                   tr("Fonts over %1 are not previewed.").arg(ImageDecoder::formatBytes(FontFile::kMaxFileBytes)),
                   tr("Open as text"), [this]() { requestOpenAsText(); });
        return;
    }
    // The face shown first is rebuilt on the worker too: a collection face
    // copies its tables (tens of MB for a CJK collection).
    const int wantFace = reload ? std::max(0, m_face) : 0;
    ++m_loadSerial;
    m_watcher.setFuture(QtConcurrent::run([path, wantFace]() {
        Loaded loaded;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            loaded.doc.error = file.errorString();
            return loaded;
        }
        loaded.doc = FontFile::parse(file.readAll());
        if (loaded.doc.isValid()) {
            loaded.face = std::min(wantFace, int(loaded.doc.faces.size()) - 1);
            loaded.faceBytes = FontFile::faceData(loaded.doc, loaded.face);
        }
        return loaded;
    }));
}

void FontPreviewWidget::onLoaded()
{
    Loaded loaded = m_watcher.result();
    FontFile::Document &doc = loaded.doc;
    if (!doc.isValid()) {
        if (m_pendingReload && m_doc.isValid()) {
            showBanner(BannerKind::Warning, tr("Could not reload the font: %1").arg(doc.error));
        } else {
            showBanner(BannerKind::Error, tr("Cannot show this font: %1").arg(doc.error), tr("Open as text"),
                       [this]() { requestOpenAsText(); });
        }
        return;
    }
    hideBanner();
    const int previousSource = m_pendingReload ? m_source->currentData().toInt() : AllCharacters;
    const int previousRow = m_pendingReload ? m_grid->scrollRow() : 0;
    m_doc = std::move(doc);

    m_faces->clear();
    for (const FontFile::Face &face : std::as_const(m_doc.faces)) {
        const QString name = !face.fullName.isEmpty() ? face.fullName
                                                      : QStringLiteral("%1 %2").arg(face.family, face.subfamily);
        m_faces->addItem(name.trimmed().isEmpty() ? tr("Face %1").arg(m_faces->count() + 1) : name.trimmed());
    }
    m_faces->setVisible(m_doc.faces.size() > 1);
    m_face = -1;
    applyFace(loaded.face, loaded.faceBytes);
    if (m_pendingReload) {
        const int sourceIndex = m_source->findData(previousSource);
        if (sourceIndex >= 0) {
            m_source->setCurrentIndex(sourceIndex);
            applySource();
        }
        m_grid->setScrollRow(previousRow);
    }
}

void FontPreviewWidget::selectFace(int index)
{
    if (index < 0 || index >= m_doc.faces.size() || index == m_face)
        return;
    if (m_doc.container != FontFile::Container::Collection) {
        applyFace(index, m_doc.data);
        return;
    }
    // Latest wins: setFuture detaches the watcher from an older rebuild.
    m_faceWatcher.setFuture(QtConcurrent::run(
        [serial = m_loadSerial, data = m_doc.data, tables = m_doc.faces[index].tables, index]() {
            return FaceBuild{serial, index, FontFile::buildFace(data, tables)};
        }));
}

void FontPreviewWidget::applyFace(int index, const QByteArray &bytes)
{
    if (index < 0 || index >= m_doc.faces.size())
        return;
    m_face = index;
    m_faces->setCurrentIndex(index);
    m_raw = QRawFont(bytes, 32.0);
    if (!m_raw.isValid()) {
        showBanner(BannerKind::Error, tr("The font's tables were read, but this system cannot render it."),
                   tr("Open as text"), [this]() { requestOpenAsText(); });
    } else {
        hideBanner();
    }
    m_sampleView->setRawFont(m_raw);
    m_sampleView->setText(m_sample->text().isEmpty() ? defaultSample() : m_sample->text());
    m_grid->setRawFont(m_raw);
    rebuildSources();
    updateInfo();
}

QString FontPreviewWidget::defaultSample() const
{
    if (m_face < 0)
        return kPangram.toString();
    const auto &cmap = m_doc.faces[m_face].cmap;
    auto has = [&cmap](char32_t cp) {
        return std::binary_search(cmap.begin(), cmap.end(), std::pair<char32_t, quint32>(cp, 0),
                                  [](const auto &a, const auto &b) { return a.first < b.first; });
    };
    if (std::all_of(kPangram.begin(), kPangram.end(), [&has](QChar c) { return c == u' ' || has(c.unicode()); }))
        return kPangram.toString();
    // A font without Latin: the first characters of its largest block.
    const auto &blocks = UnicodeBlocks::all();
    std::pair<CmapIt, CmapIt> best{cmap.end(), cmap.end()};
    for (const UnicodeBlocks::Block &block : blocks) {
        const auto range = blockRange(cmap, block);
        if (range.second - range.first > best.second - best.first)
            best = range;
    }
    if (best.first == best.second)
        best = {cmap.begin(), cmap.begin() + std::min<std::ptrdiff_t>(32, std::ssize(cmap))};
    QString sample;
    for (auto it = best.first; it != best.second && sample.size() < 48; ++it)
        sample += QString::fromUcs4(&it->first, 1);
    return sample;
}

void FontPreviewWidget::rebuildSources()
{
    m_source->clear();
    if (m_face < 0)
        return;
    const FontFile::Face &face = m_doc.faces[m_face];
    const QLocale locale;
    const auto &cmap = face.cmap;
    m_source->addItem(tr("All characters (%1)").arg(locale.toString(qulonglong(cmap.size()))), int(AllCharacters));
    qsizetype inBlocks = 0;
    const auto &blocks = UnicodeBlocks::all();
    for (int i = 0; i < int(blocks.size()); ++i) {
        const auto range = blockRange(cmap, blocks[size_t(i)]);
        const auto count = range.second - range.first;
        if (count == 0)
            continue;
        inBlocks += count;
        m_source->addItem(QStringLiteral("%1 (%2)").arg(QString::fromLatin1(blocks[size_t(i)].name),
                                                          locale.toString(qlonglong(count))),
                          i);
    }
    const qsizetype other = qsizetype(cmap.size()) - inBlocks;
    if (other > 0)
        m_source->addItem(tr("Other characters (%1)").arg(locale.toString(qlonglong(other))), int(OtherCharacters));
    if (face.numGlyphs > 0)
        m_source->addItem(tr("All glyphs by id (%1)").arg(locale.toString(face.numGlyphs)), int(AllGlyphs));
    m_source->setCurrentIndex(0);
    applySource();
}

void FontPreviewWidget::applySource()
{
    std::vector<GlyphGridView::Cell> cells;
    if (m_face >= 0) {
        const FontFile::Face &face = m_doc.faces[m_face];
        const auto &cmap = face.cmap;
        const int source = m_source->currentData().toInt();
        if (source == AllGlyphs) {
            // Each glyph with the first code point mapping to it, if any.
            std::vector<char32_t> codePoints(size_t(face.numGlyphs), 0);
            for (const auto &[cp, glyph] : cmap) {
                if (glyph < codePoints.size() && codePoints[glyph] == 0)
                    codePoints[glyph] = cp;
            }
            cells.reserve(codePoints.size());
            for (quint32 g = 0; g < codePoints.size(); ++g)
                cells.push_back({codePoints[g], g});
        } else if (source >= 0 && source < int(UnicodeBlocks::all().size())) {
            const auto range = blockRange(cmap, UnicodeBlocks::all()[size_t(source)]);
            cells.reserve(size_t(range.second - range.first));
            for (auto it = range.first; it != range.second; ++it)
                cells.push_back({it->first, it->second});
        } else {
            const bool otherOnly = source == OtherCharacters;
            cells.reserve(cmap.size());
            for (const auto &[cp, glyph] : cmap) {
                if (!otherOnly || UnicodeBlocks::indexOf(quint32(cp)) < 0)
                    cells.push_back({cp, glyph});
            }
        }
    }
    m_grid->setCells(std::move(cells));
}

void FontPreviewWidget::updateInfo()
{
    if (m_face < 0) {
        setInfoText(QString());
        return;
    }
    const FontFile::Face &face = m_doc.faces[m_face];
    const QLocale locale;
    QStringList parts;
    parts << QStringLiteral("%1 %2").arg(face.family, face.subfamily).trimmed();
    QString format;
    switch (m_doc.container) {
    case FontFile::Container::Collection: format = tr("Collection (%n faces)", nullptr, int(m_doc.faces.size())); break;
    case FontFile::Container::Woff: format = QStringLiteral("WOFF"); break;
    default: format = face.outlines == QLatin1String("TrueType") ? QStringLiteral("TrueType")
                                                                  : QStringLiteral("OpenType"); break;
    }
    if (!face.outlines.isEmpty() && !format.startsWith(face.outlines))
        format += QStringLiteral(" (%1)").arg(face.outlines);
    parts << format;
    if (!face.version.isEmpty())
        parts << face.version;
    parts << tr("%1 glyphs").arg(locale.toString(face.numGlyphs));
    if (face.weightClass > 0)
        parts << tr("weight %1").arg(face.weightClass);
    if (!face.axes.isEmpty()) {
        QStringList axes;
        for (const FontFile::Axis &a : face.axes)
            axes << QStringLiteral("%1 %2–%3").arg(a.tag, locale.toString(a.min, 'g', 6), locale.toString(a.max, 'g', 6));
        parts << tr("variable: %1").arg(axes.join(QStringLiteral(", ")));
    }
    if (face.colour)
        parts << tr("colour glyphs");
    parts << ImageDecoder::formatBytes(m_fileSize);
    m_infoText = parts.join(QStringLiteral("  ·  "));
    setInfoText(m_infoText);

    QStringList tip;
    auto add = [&tip](const QString &label, const QString &value) {
        if (!value.isEmpty())
            tip << label.arg(truncated(value, 400));
    };
    add(tr("Full name: %1"), face.fullName);
    add(tr("PostScript name: %1"), face.postScriptName);
    add(tr("Designer: %1"), face.designer);
    add(tr("Copyright: %1"), face.copyright);
    add(tr("License: %1"), face.license);
    if (face.unitsPerEm > 0)
        tip << tr("Units per em: %1").arg(face.unitsPerEm);
    if (face.hasShaping)
        tip << tr("The sample applies kerning only; ligatures and complex scripts are not shaped.");
    setInfoToolTip(tip.join(QLatin1Char('\n')));
}

void FontPreviewWidget::showHover(int index)
{
    const auto &cells = m_grid->cells();
    if (index < 0 || index >= int(cells.size())) {
        setInfoText(m_infoText);
        return;
    }
    const GlyphGridView::Cell &cell = cells[size_t(index)];
    QStringList parts;
    if (cell.codePoint != 0) {
        parts << QStringLiteral("U+%1").arg(quint32(cell.codePoint), 4, 16, QLatin1Char('0')).toUpper();
        const int block = UnicodeBlocks::indexOf(quint32(cell.codePoint));
        if (block >= 0)
            parts << QString::fromLatin1(UnicodeBlocks::all()[size_t(block)].name);
    }
    parts << tr("glyph %1").arg(cell.glyph);
    if (m_raw.isValid()) {
        const quint32 glyph = cell.glyph;
        QPointF advance;
        if (m_raw.advancesForGlyphIndexes(&glyph, &advance, 1) && m_raw.unitsPerEm() > 0 && m_raw.pixelSize() > 0) {
            const double units = advance.x() * m_raw.unitsPerEm() / m_raw.pixelSize();
            parts << tr("advance %1").arg(qRound(units));
        }
    }
    setInfoText(parts.join(QStringLiteral("  ·  ")));
}
