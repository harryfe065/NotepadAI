#include "AtlasLoader.h"

#include "AssetFileUtil.h"
#include "ImageDecoder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QVariant>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Atlas;

namespace {

constexpr int kMaxItems = 2'000'000;
constexpr int kMaxPlistDepth = 256;   // plist nesting; deeper is refused

using AssetFileUtil::trimmedHead;

bool has(QByteArrayView hay, const char *needle)
{
    return hay.indexOf(QByteArrayView(needle)) >= 0;
}

QVector<double> numbersIn(const QString &s)
{
    QVector<double> out;
    int i = 0;
    const int n = int(s.size());
    while (i < n) {
        const QChar c = s[i];
        if (c.isDigit() || ((c == QLatin1Char('-') || c == QLatin1Char('.')) && i + 1 < n && (s[i + 1].isDigit() || s[i + 1] == QLatin1Char('.')))) {
            int j = i + 1;
            while (j < n && (s[j].isDigit() || s[j] == QLatin1Char('.')))
                ++j;
            out.append(s.mid(i, j - i).toDouble());
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

// "{{x,y},{w,h}}" -> rect, "{x,y}" -> point / size via numbersIn().
QRect rectOf(const QString &s)
{
    const QVector<double> v = numbersIn(s);
    return v.size() >= 4 ? QRect(int(std::lround(v[0])), int(std::lround(v[1])), int(std::lround(v[2])), int(std::lround(v[3])))
                         : QRect();
}

int toInt(const QJsonValue &v, int def = 0)
{
    return v.isDouble() ? int(std::lround(v.toDouble())) : def;
}

QRect jsonRect(const QJsonObject &o)
{
    return QRect(toInt(o.value(QLatin1String("x"))), toInt(o.value(QLatin1String("y"))),
                 toInt(o.value(QLatin1String("w"))), toInt(o.value(QLatin1String("h"))));
}

void resolvePage(Page &page, const QString &baseDir)
{
    page.path.clear();
    // A network path written in the file is treated as missing: probing it
    // would make Windows connect over SMB.
    if (page.file.isEmpty() || AssetFileUtil::isNetworkPath(page.file))
        return;
    const QDir dir(baseDir);
    QString file = page.file;
    file.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QStringList candidates{dir.absoluteFilePath(file), dir.absoluteFilePath(QFileInfo(file).fileName())};
    static const QStringList kExt = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                     QStringLiteral("webp"), QStringLiteral("tga"), QStringLiteral("dds"),
                                     QStringLiteral("bmp"), QStringLiteral("tif"), QStringLiteral("tiff")};
    const QString base = QFileInfo(file).completeBaseName();
    for (const QString &ext : kExt)
        candidates << dir.absoluteFilePath(base + QLatin1Char('.') + ext);
    for (const QString &c : candidates) {
        if (QFileInfo(c).isFile()) {
            page.path = c;
            return;
        }
    }
}

// ------------------------------------------------------------------ libGDX

std::shared_ptr<Document> parseGdx(const QString &text, QString *error)
{
    auto doc = std::make_shared<Document>();
    doc->format = QStringLiteral("libGDX / Spine atlas");

    struct Raw {
        int x = 0, y = 0, w = 0, h = 0;
        int ow = 0, oh = 0, ox = 0, oy = 0;
        bool hasOrig = false;
    } raw;
    Frame current;
    bool open = false;

    auto flush = [&]() {
        if (!open)
            return;
        open = false;
        if (raw.w <= 0 && raw.h <= 0)
            return;                 // a stray line, not a region
        // `size` is the region as shown; a rotated region occupies h x w on the sheet.
        current.rect = current.rotated ? QRect(raw.x, raw.y, raw.h, raw.w) : QRect(raw.x, raw.y, raw.w, raw.h);
        const int ow = raw.hasOrig ? raw.ow : raw.w, oh = raw.hasOrig ? raw.oh : raw.h;
        current.sourceSize = QSize(ow, oh);
        // The offset's y runs from the bottom edge.
        current.trim = QPoint(raw.ox, oh - raw.h - raw.oy);
        doc->frames.push_back(current);
    };

    bool expectPage = true;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        QString line = rawLine;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            flush();
            expectPage = true;
            continue;
        }
        const bool indented = line.startsWith(QLatin1Char(' ')) || line.startsWith(QLatin1Char('\t'));
        const int colon = int(trimmed.indexOf(QLatin1Char(':')));
        if (!indented) {
            if (expectPage) {
                flush();
                Page page;
                page.file = trimmed;
                doc->pages.push_back(page);
                expectPage = false;
                continue;
            }
            if (colon > 0) {
                const QString key = trimmed.left(colon).trimmed();
                const QVector<double> v = numbersIn(trimmed.mid(colon + 1));
                if (key == QLatin1String("size") && v.size() >= 2 && !doc->pages.empty())
                    doc->pages.back().size = QSize(int(v[0]), int(v[1]));
                continue;
            }
            flush();
            current = Frame();
            raw = Raw();
            current.name = trimmed;
            current.page = int(doc->pages.size()) - 1;
            open = true;
            if (int(doc->frames.size()) > kMaxItems)
                break;
            continue;
        }
        if (!open || colon < 0)
            continue;
        const QString key = trimmed.left(colon).trimmed();
        const QString value = trimmed.mid(colon + 1).trimmed();
        const QVector<double> v = numbersIn(value);
        if (key == QLatin1String("rotate")) {
            current.rotated = value == QLatin1String("true") || (v.size() == 1 && int(v[0]) == 90);
        } else if (key == QLatin1String("xy") && v.size() >= 2) {
            raw.x = int(v[0]);
            raw.y = int(v[1]);
        } else if (key == QLatin1String("size") && v.size() >= 2) {
            raw.w = int(v[0]);
            raw.h = int(v[1]);
        } else if (key == QLatin1String("bounds") && v.size() >= 4) {
            raw.x = int(v[0]);
            raw.y = int(v[1]);
            raw.w = int(v[2]);
            raw.h = int(v[3]);
        } else if (key == QLatin1String("orig") && v.size() >= 2) {
            raw.ow = int(v[0]);
            raw.oh = int(v[1]);
            raw.hasOrig = true;
        } else if (key == QLatin1String("offset") && v.size() >= 2) {
            raw.ox = int(v[0]);
            raw.oy = int(v[1]);
        } else if (key == QLatin1String("offsets") && v.size() >= 4) {
            raw.ox = int(v[0]);
            raw.oy = int(v[1]);
            raw.ow = int(v[2]);
            raw.oh = int(v[3]);
            raw.hasOrig = true;
        } else if (key == QLatin1String("index") && v.size() >= 1) {
            current.index = int(v[0]);
        }
    }
    flush();
    if (doc->frames.empty()) {
        if (error) *error = QObject::tr("No regions found in this atlas.");
        return nullptr;
    }
    return doc;
}

// -------------------------------------------------------------------- JSON

void addJsonFrame(Document &doc, int page, const QString &fallbackName, const QJsonObject &f, bool godot)
{
    Frame fr;
    fr.page = page;
    fr.name = f.value(QLatin1String("filename")).toString(fallbackName);
    if (godot) {
        fr.rect = jsonRect(f.value(QLatin1String("region")).toObject());
        const QJsonObject margin = f.value(QLatin1String("margin")).toObject();
        fr.trim = QPoint(toInt(margin.value(QLatin1String("x"))), toInt(margin.value(QLatin1String("y"))));
        fr.sourceSize = fr.rect.size();
    } else {
        fr.rect = jsonRect(f.value(QLatin1String("frame")).toObject());
        fr.rotated = f.value(QLatin1String("rotated")).toBool();
        const QJsonObject sss = f.value(QLatin1String("spriteSourceSize")).toObject();
        fr.trim = QPoint(toInt(sss.value(QLatin1String("x"))), toInt(sss.value(QLatin1String("y"))));
        const QJsonObject ss = f.value(QLatin1String("sourceSize")).toObject();
        fr.sourceSize = QSize(toInt(ss.value(QLatin1String("w"))), toInt(ss.value(QLatin1String("h"))));
        if (fr.sourceSize.isEmpty())
            fr.sourceSize = fr.rotated ? QSize(fr.rect.height(), fr.rect.width()) : fr.rect.size();
        const QJsonObject pv = f.value(QLatin1String("pivot")).toObject();
        if (pv.contains(QLatin1String("x"))) {
            fr.pivot = QPointF(pv.value(QLatin1String("x")).toDouble(), pv.value(QLatin1String("y")).toDouble());
            fr.hasPivot = true;
        }
    }
    doc.frames.push_back(fr);
}

std::shared_ptr<Document> parseJson(const QJsonObject &root, QString *error)
{
    auto doc = std::make_shared<Document>();
    const QJsonObject meta = root.value(QLatin1String("meta")).toObject();
    const QString app = meta.value(QLatin1String("app")).toString().toLower();
    doc->format = app.contains(QLatin1String("aseprite")) ? QStringLiteral("Aseprite sheet")
                  : app.contains(QLatin1String("texturepacker")) ? QStringLiteral("TexturePacker JSON")
                                                                 : QStringLiteral("JSON atlas");

    auto addFrames = [&](const QJsonValue &frames, int page, bool godot) {
        if (frames.isArray()) {
            for (const QJsonValue &fv : frames.toArray()) {
                if (int(doc->frames.size()) > kMaxItems)
                    return;
                addJsonFrame(*doc, page, QString(), fv.toObject(), godot);
            }
        } else if (frames.isObject()) {
            const QJsonObject o = frames.toObject();
            for (auto it = o.constBegin(); it != o.constEnd(); ++it)
                addJsonFrame(*doc, page, it.key(), it.value().toObject(), godot);
        }
    };

    if (root.contains(QLatin1String("textures"))) {
        for (const QJsonValue &tv : root.value(QLatin1String("textures")).toArray()) {
            const QJsonObject t = tv.toObject();
            Page page;
            page.file = t.value(QLatin1String("image")).toString();
            const QJsonObject size = t.value(QLatin1String("size")).toObject();
            page.size = QSize(toInt(size.value(QLatin1String("w"))), toInt(size.value(QLatin1String("h"))));
            doc->pages.push_back(page);
            const int index = int(doc->pages.size()) - 1;
            const bool godot = t.contains(QLatin1String("sprites"));
            if (godot)
                doc->format = QStringLiteral("Godot sprite sheet");
            addFrames(godot ? t.value(QLatin1String("sprites")) : t.value(QLatin1String("frames")), index, godot);
        }
    } else if (root.contains(QLatin1String("frames"))) {
        Page page;
        page.file = meta.value(QLatin1String("image")).toString();
        const QJsonObject size = meta.value(QLatin1String("size")).toObject();
        page.size = QSize(toInt(size.value(QLatin1String("w"))), toInt(size.value(QLatin1String("h"))));
        doc->pages.push_back(page);
        addFrames(root.value(QLatin1String("frames")), 0, false);
    }

    // Aseprite animation tags label their frame ranges; a later tag wins where
    // ranges overlap. Tags are applied last to first, and `next` (a
    // path-halving union-find) skips frames already labelled, so every frame
    // is written once: O((frames + tags) α) however the ranges overlap.
    const QJsonArray tags = meta.value(QLatin1String("frameTags")).toArray();
    if (root.value(QLatin1String("frames")).isArray() && !tags.isEmpty() && !doc->frames.empty()) {
        const int n = int(doc->frames.size());
        std::vector<int> next(size_t(n) + 1);
        for (int i = 0; i <= n; ++i)
            next[size_t(i)] = i;
        auto find = [&next](int i) {
            while (next[size_t(i)] != i) {
                next[size_t(i)] = next[size_t(next[size_t(i)])];
                i = next[size_t(i)];
            }
            return i;
        };
        for (qsizetype k = tags.size(); k-- > 0;) {
            const QJsonObject t = tags.at(k).toObject();
            const int from = std::max(0, toInt(t.value(QLatin1String("from")), -1));
            const int to = std::min(n - 1, toInt(t.value(QLatin1String("to")), -1));
            if (from > to)
                continue;
            const QString name = t.value(QLatin1String("name")).toString();
            for (int i = find(from); i <= to; i = find(i)) {
                doc->frames[size_t(i)].tag = name;
                next[size_t(i)] = i + 1;
            }
        }
    }

    if (doc->frames.empty()) {
        if (error) *error = QObject::tr("No frames found in this JSON file.");
        return nullptr;
    }
    return doc;
}

// ------------------------------------------------------------------- plist

// Recursive per dict / array; QXmlStreamReader has no depth limit, so past
// kMaxPlistDepth the reader is put in the error state (which ends every
// level's loop) instead of overflowing the stack.
QVariant readPlistValue(QXmlStreamReader &xml, int depth = 0)
{
    const QString name = xml.name().toString();
    const bool container = name == QLatin1String("dict") || name == QLatin1String("array");
    if (container && depth >= kMaxPlistDepth) {
        xml.raiseError(QObject::tr("The property list is nested more than %1 levels deep.").arg(kMaxPlistDepth));
        return {};
    }
    if (name == QLatin1String("dict")) {
        QVariantMap m;
        QString key;
        while (xml.readNextStartElement()) {
            if (xml.name() == QLatin1String("key"))
                key = xml.readElementText();
            else
                m.insert(key, readPlistValue(xml, depth + 1));
        }
        return m;
    }
    if (name == QLatin1String("array")) {
        QVariantList l;
        while (xml.readNextStartElement())
            l << readPlistValue(xml, depth + 1);
        return l;
    }
    if (name == QLatin1String("true") || name == QLatin1String("false")) {
        xml.skipCurrentElement();
        return name == QLatin1String("true");
    }
    if (name == QLatin1String("integer") || name == QLatin1String("real"))
        return xml.readElementText().toDouble();
    return xml.readElementText();
}

std::shared_ptr<Document> parsePlist(const QByteArray &data, QString *error)
{
    QXmlStreamReader xml(data);
    QVariantMap root;
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("plist")) {
            if (xml.readNextStartElement())
                root = readPlistValue(xml).toMap();
            break;
        }
        xml.skipCurrentElement();
    }
    const QVariantMap frames = root.value(QStringLiteral("frames")).toMap();
    if (xml.hasError() || frames.isEmpty()) {
        if (error) *error = QObject::tr("This property list has no frames.");
        return nullptr;
    }
    auto doc = std::make_shared<Document>();
    doc->format = QStringLiteral("Cocos2d plist");

    const QVariantMap meta = root.value(QStringLiteral("metadata")).toMap();
    Page page;
    page.file = meta.value(QStringLiteral("realTextureFileName")).toString();
    if (page.file.isEmpty())
        page.file = meta.value(QStringLiteral("textureFileName")).toString();
    const QVector<double> size = numbersIn(meta.value(QStringLiteral("size")).toString());
    if (size.size() >= 2)
        page.size = QSize(int(size[0]), int(size[1]));
    doc->pages.push_back(page);

    for (auto it = frames.constBegin(); it != frames.constEnd(); ++it) {
        if (int(doc->frames.size()) > kMaxItems)
            break;
        const QVariantMap f = it.value().toMap();
        Frame fr;
        fr.name = it.key();
        auto str = [&f](const char *k) { return f.value(QLatin1String(k)).toString(); };
        auto num = [&f](const char *k) { return f.value(QLatin1String(k)).toDouble(); };
        if (f.contains(QStringLiteral("textureRect"))) {
            // Formats 2 and 3: sizes are unrotated; a rotated sprite occupies h x w.
            fr.rect = rectOf(str("textureRect"));
            fr.rotated = f.value(QStringLiteral("textureRotated")).toBool();
            if (fr.rotated)
                fr.rect = QRect(fr.rect.x(), fr.rect.y(), fr.rect.height(), fr.rect.width());
            const QVector<double> src = numbersIn(str("spriteSourceSize"));
            const QVector<double> off = numbersIn(str("spriteOffset"));
            const QVector<double> sz = numbersIn(str("spriteSize"));
            if (src.size() >= 2) fr.sourceSize = QSize(int(src[0]), int(src[1]));
            if (off.size() >= 2 && src.size() >= 2 && sz.size() >= 2) {
                // spriteOffset is the trimmed box's centre offset from the source centre (y up).
                fr.trim = QPoint(int(std::lround(src[0] / 2.0 + off[0] - sz[0] / 2.0)),
                                 int(std::lround(src[1] / 2.0 - off[1] - sz[1] / 2.0)));
            }
        } else if (f.contains(QStringLiteral("frame"))) {
            // Format 1.
            fr.rect = rectOf(str("frame"));
            fr.rotated = f.value(QStringLiteral("rotated")).toBool();
            if (fr.rotated)
                fr.rect = QRect(fr.rect.x(), fr.rect.y(), fr.rect.height(), fr.rect.width());
            const QVector<double> src = numbersIn(str("sourceSize"));
            if (src.size() >= 2) fr.sourceSize = QSize(int(src[0]), int(src[1]));
            const QRect colorRect = rectOf(str("sourceColorRect"));
            fr.trim = colorRect.topLeft();
        } else {
            // Format 0.
            fr.rect = QRect(int(num("x")), int(num("y")), int(num("width")), int(num("height")));
            fr.sourceSize = QSize(int(num("originalWidth")), int(num("originalHeight")));
            fr.trim = QPoint(int(num("offsetX")), int(num("offsetY")));
        }
        if (fr.sourceSize.isEmpty())
            fr.sourceSize = fr.rotated ? QSize(fr.rect.height(), fr.rect.width()) : fr.rect.size();
        doc->frames.push_back(fr);
    }
    return doc;
}

// ---------------------------------------------------------------- Starling

std::shared_ptr<Document> parseStarling(const QByteArray &data, QString *error)
{
    QXmlStreamReader xml(data);
    auto doc = std::make_shared<Document>();
    doc->format = QStringLiteral("Starling / Sparrow atlas");
    Page page;
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        const QXmlStreamAttributes a = xml.attributes();
        if (xml.name() == QLatin1String("TextureAtlas")) {
            page.file = a.value(QLatin1String("imagePath")).toString();
        } else if (xml.name() == QLatin1String("SubTexture")) {
            if (int(doc->frames.size()) > kMaxItems)
                break;
            Frame fr;
            fr.name = a.value(QLatin1String("name")).toString();
            const int w = a.value(QLatin1String("width")).toInt(), h = a.value(QLatin1String("height")).toInt();
            fr.rotated = a.value(QLatin1String("rotated")).toString() == QLatin1String("true");
            // Sizes are the sprite's own; a rotated one occupies h x w on the sheet.
            fr.rect = QRect(a.value(QLatin1String("x")).toInt(), a.value(QLatin1String("y")).toInt(),
                            fr.rotated ? h : w, fr.rotated ? w : h);
            fr.trim = QPoint(-a.value(QLatin1String("frameX")).toInt(), -a.value(QLatin1String("frameY")).toInt());
            const int fw = a.value(QLatin1String("frameWidth")).toInt(), fh = a.value(QLatin1String("frameHeight")).toInt();
            fr.sourceSize = (fw > 0 && fh > 0) ? QSize(fw, fh) : QSize(w, h);
            if (a.hasAttribute(QLatin1String("pivotX"))) {
                fr.pivot = QPointF(a.value(QLatin1String("pivotX")).toDouble(), a.value(QLatin1String("pivotY")).toDouble());
                fr.hasPivot = true;
            }
            doc->frames.push_back(fr);
        }
    }
    doc->pages.push_back(page);
    if (xml.hasError() || doc->frames.empty()) {
        if (error) *error = QObject::tr("No sub-textures found in this atlas.");
        return nullptr;
    }
    return doc;
}

// ------------------------------------------------------------------ BMFont

// key=value tokens of one text line; values may be "quoted with spaces".
QHash<QString, QString> keyValues(const QString &line)
{
    QHash<QString, QString> out;
    int i = 0;
    const int n = int(line.size());
    while (i < n) {
        while (i < n && line[i].isSpace()) ++i;
        int k = i;
        while (i < n && line[i] != QLatin1Char('=') && !line[i].isSpace()) ++i;
        const QString key = line.mid(k, i - k);
        if (i >= n || line[i] != QLatin1Char('=')) {
            continue;
        }
        ++i;
        QString value;
        if (i < n && line[i] == QLatin1Char('"')) {
            ++i;
            const int start = i;
            while (i < n && line[i] != QLatin1Char('"')) ++i;
            value = line.mid(start, i - start);
            if (i < n) ++i;
        } else {
            const int start = i;
            while (i < n && !line[i].isSpace()) ++i;
            value = line.mid(start, i - start);
        }
        out.insert(key, value);
    }
    return out;
}

Glyph glyphFrom(const QHash<QString, QString> &kv)
{
    Glyph g;
    g.id = kv.value(QStringLiteral("id")).toUInt();
    g.rect = QRect(kv.value(QStringLiteral("x")).toInt(), kv.value(QStringLiteral("y")).toInt(),
                   kv.value(QStringLiteral("width")).toInt(), kv.value(QStringLiteral("height")).toInt());
    g.xoffset = kv.value(QStringLiteral("xoffset")).toInt();
    g.yoffset = kv.value(QStringLiteral("yoffset")).toInt();
    g.xadvance = kv.value(QStringLiteral("xadvance")).toInt();
    g.page = kv.value(QStringLiteral("page")).toInt();
    return g;
}

void addPage(Document &doc, int id, const QString &file)
{
    if (id < 0 || id > 4096)
        return;
    if (size_t(id) >= doc.pages.size())
        doc.pages.resize(size_t(id) + 1);
    doc.pages[size_t(id)].file = file;
}

std::shared_ptr<Document> parseBmFontText(const QString &text, QString *error)
{
    auto doc = std::make_shared<Document>();
    doc->kind = Document::Kind::BitmapFont;
    doc->format = QStringLiteral("BMFont (text)");
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        const int space = int(line.indexOf(QLatin1Char(' ')));
        const QString type = space > 0 ? line.left(space) : line;
        const QHash<QString, QString> kv = keyValues(line);
        if (type == QLatin1String("info")) {
            doc->face = kv.value(QStringLiteral("face"));
            doc->fontSize = std::abs(kv.value(QStringLiteral("size")).toInt());
        } else if (type == QLatin1String("common")) {
            doc->lineHeight = kv.value(QStringLiteral("lineHeight")).toInt();
            doc->base = kv.value(QStringLiteral("base")).toInt();
        } else if (type == QLatin1String("page")) {
            addPage(*doc, kv.value(QStringLiteral("id")).toInt(), kv.value(QStringLiteral("file")));
        } else if (type == QLatin1String("char")) {
            if (int(doc->glyphs.size()) < kMaxItems)
                doc->glyphs.push_back(glyphFrom(kv));
        } else if (type == QLatin1String("kerning")) {
            if (int(doc->kernings.size()) < kMaxItems)
                doc->kernings.push_back({kv.value(QStringLiteral("first")).toUInt(),
                                         kv.value(QStringLiteral("second")).toUInt(),
                                         kv.value(QStringLiteral("amount")).toInt()});
        }
    }
    if (doc->glyphs.empty()) {
        if (error) *error = QObject::tr("No characters found in this bitmap font.");
        return nullptr;
    }
    doc->indexFont();
    return doc;
}

std::shared_ptr<Document> parseBmFontXml(const QByteArray &data, QString *error)
{
    auto doc = std::make_shared<Document>();
    doc->kind = Document::Kind::BitmapFont;
    doc->format = QStringLiteral("BMFont (XML)");
    QXmlStreamReader xml(data);
    auto kvOf = [](const QXmlStreamAttributes &a) {
        QHash<QString, QString> kv;
        for (const QXmlStreamAttribute &at : a)
            kv.insert(at.name().toString(), at.value().toString());
        return kv;
    };
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        const QHash<QString, QString> kv = kvOf(xml.attributes());
        const QStringView name = xml.name();
        if (name == QLatin1String("info")) {
            doc->face = kv.value(QStringLiteral("face"));
            doc->fontSize = std::abs(kv.value(QStringLiteral("size")).toInt());
        } else if (name == QLatin1String("common")) {
            doc->lineHeight = kv.value(QStringLiteral("lineHeight")).toInt();
            doc->base = kv.value(QStringLiteral("base")).toInt();
        } else if (name == QLatin1String("page")) {
            addPage(*doc, kv.value(QStringLiteral("id")).toInt(), kv.value(QStringLiteral("file")));
        } else if (name == QLatin1String("char")) {
            if (int(doc->glyphs.size()) < kMaxItems)
                doc->glyphs.push_back(glyphFrom(kv));
        } else if (name == QLatin1String("kerning")) {
            if (int(doc->kernings.size()) < kMaxItems)
                doc->kernings.push_back({kv.value(QStringLiteral("first")).toUInt(),
                                         kv.value(QStringLiteral("second")).toUInt(),
                                         kv.value(QStringLiteral("amount")).toInt()});
        }
    }
    if (xml.hasError() || doc->glyphs.empty()) {
        if (error) *error = QObject::tr("No characters found in this bitmap font.");
        return nullptr;
    }
    doc->indexFont();
    return doc;
}

struct Reader {
    const unsigned char *p;
    size_t n;
    bool ok(size_t off, size_t len) const { return off + len <= n; }
    quint32 u32(size_t off) const
    {
        return quint32(p[off]) | (quint32(p[off + 1]) << 8) | (quint32(p[off + 2]) << 16) | (quint32(p[off + 3]) << 24);
    }
    quint16 u16(size_t off) const { return quint16(p[off] | (p[off + 1] << 8)); }
    qint16 i16(size_t off) const { return qint16(u16(off)); }
};

std::shared_ptr<Document> parseBmFontBinary(const QByteArray &data, QString *error)
{
    auto doc = std::make_shared<Document>();
    doc->kind = Document::Kind::BitmapFont;
    doc->format = QStringLiteral("BMFont (binary)");
    const Reader r{reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size())};
    size_t off = 4;
    while (r.ok(off, 5)) {
        const int type = r.p[off];
        const size_t size = r.u32(off + 1);
        off += 5;
        if (!r.ok(off, size))
            break;
        switch (type) {
        case 1:   // info
            if (size >= 15) {
                doc->fontSize = std::abs(int(r.i16(off)));
                doc->face = QString::fromUtf8(reinterpret_cast<const char *>(r.p + off + 14),
                                              qsizetype(strnlen(reinterpret_cast<const char *>(r.p + off + 14), size - 14)));
            }
            break;
        case 2:   // common
            if (size >= 6) {
                doc->lineHeight = r.u16(off);
                doc->base = r.u16(off + 2);
            }
            break;
        case 3: { // pages: equal-length null-terminated names
            size_t i = 0, id = 0;
            while (i < size) {
                const size_t len = strnlen(reinterpret_cast<const char *>(r.p + off + i), size - i);
                addPage(*doc, int(id++), QString::fromUtf8(reinterpret_cast<const char *>(r.p + off + i), qsizetype(len)));
                i += len + 1;
            }
            break;
        }
        case 4:   // chars, 20 bytes each
            for (size_t i = 0; i + 20 <= size && int(doc->glyphs.size()) < kMaxItems; i += 20) {
                Glyph g;
                g.id = r.u32(off + i);
                g.rect = QRect(r.u16(off + i + 4), r.u16(off + i + 6), r.u16(off + i + 8), r.u16(off + i + 10));
                g.xoffset = r.i16(off + i + 12);
                g.yoffset = r.i16(off + i + 14);
                g.xadvance = r.i16(off + i + 16);
                g.page = r.p[off + i + 18];
                doc->glyphs.push_back(g);
            }
            break;
        case 5:   // kerning pairs, 10 bytes each
            for (size_t i = 0; i + 10 <= size && int(doc->kernings.size()) < kMaxItems; i += 10)
                doc->kernings.push_back({r.u32(off + i), r.u32(off + i + 4), int(r.i16(off + i + 8))});
            break;
        default:
            break;
        }
        off += size;
    }
    if (doc->glyphs.empty()) {
        if (error) *error = QObject::tr("No characters found in this bitmap font.");
        return nullptr;
    }
    doc->indexFont();
    return doc;
}

} // namespace

namespace AtlasLoader {

bool looksLikeBmFont(QByteArrayView head)
{
    head = trimmedHead(head);
    if (head.size() >= 4 && head.startsWith("BMF") && static_cast<unsigned char>(head[3]) == 3)
        return true;
    if (head.startsWith("info ") && has(head.first(std::min<qsizetype>(head.size(), 400)), "face="))
        return true;
    if (head.startsWith("<")) {
        const QByteArrayView first = head.first(std::min<qsizetype>(head.size(), 1024));
        return has(first, "<font") && (has(head, "<info ") || has(head, "<common "));
    }
    return false;
}

bool looksLikeAtlasContent(QByteArrayView head)
{
    if (looksLikeBmFont(head))
        return true;
    head = trimmedHead(head);
    if (head.isEmpty())
        return false;
    if (head[0] == '{') {
        // TexturePacker puts "frames" first and "meta" last, so the head may
        // only hold the first frames.
        return (has(head, "\"frames\"") && has(head, "\"frame\""))
               || (has(head, "\"textures\"") && (has(head, "\"frames\"") || has(head, "\"sprites\"")));
    }
    if (head[0] == '<')
        return has(head, "<TextureAtlas") || (has(head, "<plist") && has(head, "<key>frames</key>"));

    // libGDX text atlas: an image name, then "size:" / "format:" lines.
    const QString text = QString::fromUtf8(head.first(std::min<qsizetype>(head.size(), 512)));
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.size() < 2)
        return false;
    const QString first = lines[0].trimmed().toLower();
    static const QStringList kExt = {QStringLiteral(".png"), QStringLiteral(".jpg"), QStringLiteral(".jpeg"),
                                     QStringLiteral(".webp"), QStringLiteral(".tga"), QStringLiteral(".dds")};
    const bool image = std::any_of(kExt.begin(), kExt.end(), [&first](const QString &e) { return first.endsWith(e); });
    const QString second = lines[1].trimmed();
    return image && (second.startsWith(QLatin1String("size:")) || second.startsWith(QLatin1String("format:"))
                     || second.startsWith(QLatin1String("filter:")));
}

std::shared_ptr<Document> parse(const QByteArray &data, const QString &suffixLower, const QString &baseDir, QString *error)
{
    Q_UNUSED(suffixLower);
    std::shared_ptr<Document> doc;
    const QByteArrayView head = trimmedHead(QByteArrayView(data).first(std::min<qsizetype>(data.size(), 4096)));

    if (head.startsWith("BMF")) {
        doc = parseBmFontBinary(data, error);
    } else if (!head.isEmpty() && head[0] == '{') {
        QJsonParseError err;
        const QJsonDocument json = QJsonDocument::fromJson(data, &err);
        if (!json.isObject()) {
            if (error) *error = QObject::tr("The file is not valid JSON: %1").arg(err.errorString());
            return nullptr;
        }
        doc = parseJson(json.object(), error);
    } else if (!head.isEmpty() && head[0] == '<') {
        if (has(head, "<TextureAtlas"))
            doc = parseStarling(data, error);
        else if (has(head, "<plist"))
            doc = parsePlist(data, error);
        else if (has(head, "<font"))
            doc = parseBmFontXml(data, error);
        else if (error)
            *error = QObject::tr("This XML file is not a texture atlas or bitmap font.");
    } else {
        // A binary that is no BMFont (a Windows .fnt / .fon resource) is not
        // text either: say so instead of parsing it as a libGDX atlas.
        if (head.contains('\0')) {
            if (error)
                *error = QObject::tr("This file is not a texture atlas or bitmap font.");
            return nullptr;
        }
        const QString text = QString::fromUtf8(data);
        if (head.startsWith("info "))
            doc = parseBmFontText(text, error);
        else
            doc = parseGdx(text, error);
    }
    if (!doc)
        return nullptr;
    for (Page &page : doc->pages)
        resolvePage(page, baseDir);
    return doc;
}

Result load(const QString &path, const std::atomic_bool &cancel)
{
    Result result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    if (file.size() > kMaxFileBytes) {
        result.error = QObject::tr("The file is larger than the %1 MB limit.").arg(kMaxFileBytes >> 20);
        return result;
    }
    const QByteArray data = file.readAll();
    QString error;
    std::shared_ptr<Document> doc = parse(data, QFileInfo(path).suffix().toLower(), QFileInfo(path).absolutePath(), &error);
    if (!doc) {
        result.error = error.isEmpty() ? QObject::tr("The file is not a texture atlas or bitmap font.") : error;
        return result;
    }

    result.images.resize(qsizetype(doc->pages.size()));
    for (size_t i = 0; i < doc->pages.size(); ++i) {
        if (cancel.load(std::memory_order_relaxed)) {
            result.cancelled = true;
            return result;
        }
        const Page &page = doc->pages[i];
        if (page.path.isEmpty()) {
            doc->warnings << QObject::tr("Page image not found: %1").arg(page.file);
            continue;
        }
        const ImageDecoder::Result r = ImageDecoder::decode(page.path, cancel);
        if (r.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!r.doc || r.doc->surfaces.empty()) {
            doc->warnings << QObject::tr("Could not read %1: %2").arg(page.file, r.error);
            continue;
        }
        result.images[qsizetype(i)] = r.doc;
    }
    result.doc = std::move(doc);
    return result;
}

} // namespace AtlasLoader
