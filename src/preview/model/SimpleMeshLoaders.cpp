#include "SimpleMeshLoaders.h"

#include "ImageDocument.h" // ImageOps::parallelRows

#include <QHash>
#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace SimpleMeshLoaders {

namespace {

using ModelLoader::Result;

constexpr qint64 kCap = ModelLoader::kMaxGeometryBytes;

inline bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// Next whitespace-delimited number; locale-independent. False at the end.
bool nextNumber(const char *&p, const char *end, double &out)
{
    while (p < end && isSpace(*p))
        ++p;
    if (p >= end)
        return false;
    const char *s = p;
    while (p < end && !isSpace(*p))
        ++p;
    if (*s == '+')
        ++s;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    const auto res = std::from_chars(s, p, out);
    if (res.ec != std::errc())
        out = 0.0;
#else
    bool ok = false;
    out = QByteArrayView(s, p - s).toDouble(&ok);
    if (!ok)
        out = 0.0;
#endif
    return true;
}

quint32 le32(const char *p)
{
    return qFromLittleEndian<quint32>(p);
}

float lef32(const char *p)
{
    const quint32 bits = qFromLittleEndian<quint32>(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

inline quint8 expand5(unsigned c)
{
    return quint8((c << 3) | (c >> 2));
}

// One mesh, one node, one primitive over the whole document.
void singleMesh(ModelDocument &doc, const ModelPrimitive &prim, const QString &name)
{
    doc.primitives.push_back(prim);
    ModelMesh mesh;
    mesh.name = name;
    mesh.primitiveCount = 1;
    doc.meshes.push_back(mesh);
    ModelNode node;
    node.name = name;
    node.mesh = 0;
    doc.nodes.push_back(node);
}

// ---------------------------------------------------------------------------
// STL

Result loadStlAscii(QByteArrayView bytes, const std::atomic_bool &cancel)
{
    auto doc = std::make_shared<ModelDocument>();
    doc->format = QObject::tr("STL ASCII");
    doc->zUp = true;
    const char *const base = bytes.data();
    const char *const end = base + bytes.size();
    qsizetype at = 0;
    quint64 found = 0;
    while (true) {
        at = bytes.indexOf(QByteArrayView("vertex"), at);
        if (at < 0)
            break;
        const char *p = base + at + 6;
        at += 6;
        double x = 0, y = 0, z = 0;
        if (!nextNumber(p, end, x) || !nextNumber(p, end, y) || !nextNumber(p, end, z))
            break;
        ModelVertex v;
        v.px = float(x);
        v.py = float(y);
        v.pz = float(z);
        doc->vertices.push_back(v);
        if ((++found & 0xFFFF) == 0) {
            if (cancel.load(std::memory_order_relaxed))
                return ModelLoader::cancelled();
            if (qint64(doc->vertices.size()) * qint64(sizeof(ModelVertex)) > kCap)
                return ModelLoader::tooLarge(qint64(doc->vertices.size()) * qint64(sizeof(ModelVertex)));
        }
    }
    doc->vertices.resize(doc->vertices.size() / 3 * 3);
    if (doc->vertices.empty())
        return ModelLoader::failed(QObject::tr("The STL file has no facets."));
    ModelPrimitive prim;
    prim.vertexCount = quint32(doc->vertices.size());
    prim.flatNormals = true;
    singleMesh(*doc, prim, QStringLiteral("STL"));
    Result r;
    r.doc = std::move(doc);
    return r;
}

} // namespace

Result loadStl(QByteArrayView bytes, const std::atomic_bool &cancel)
{
    const qint64 size = bytes.size();
    quint32 count = 0;
    bool binary = false;
    if (size >= 84) {
        count = le32(bytes.data() + 80);
        binary = 84 + qint64(count) * 50 == size;
    }
    qsizetype lead = 0;
    while (lead < bytes.size() && isSpace(bytes[lead]))
        ++lead;
    const bool asciiHeader = bytes.sliced(lead).startsWith("solid");
    if (!binary && asciiHeader)
        return loadStlAscii(bytes, cancel);
    // Some exporters pad binary files; accept when the facets fit.
    if (!binary && !(size >= 84 && count > 0 && 84 + qint64(count) * 50 <= size))
        return ModelLoader::failed(QObject::tr("Not a valid STL file."));
    if (count == 0)
        return ModelLoader::failed(QObject::tr("The STL file has no facets."));
    const qint64 need = qint64(count) * 3 * qint64(sizeof(ModelVertex));
    if (need > kCap)
        return ModelLoader::tooLarge(need);

    auto doc = std::make_shared<ModelDocument>();
    doc->format = QObject::tr("STL binary");
    doc->zUp = true;
    doc->vertices.resize(size_t(count) * 3);

    // Facet colours: Materialise ("COLOR=" header, bit 15 clear = own colour,
    // RGB from bit 0) or VisCAM / SolidView (bit 15 set = valid, BGR).
    const QByteArrayView header = bytes.first(80);
    const qsizetype colorAt = header.indexOf(QByteArrayView("COLOR="));
    const bool materialise = colorAt >= 0 && colorAt + 10 <= 80;
    quint32 defaultColor = 0xFFFFFFFFu;
    if (materialise) {
        const auto *c = reinterpret_cast<const uchar *>(header.data() + colorAt + 6);
        defaultColor = ModelPack::colorFromBytes(c[0], c[1], c[2], 255);
    }
    std::atomic_bool anyColor{materialise};

    const char *tris = bytes.data() + 84;
    ModelVertex *out = doc->vertices.data();
    ImageOps::parallelRows(int(count), qint64(count) * 3, [&](int t0, int t1) {
        bool colored = false;
        for (int t = t0; t < t1; ++t) {
            const char *f = tris + qint64(t) * 50 + 12; // skip the stored normal
            const quint16 attr = qFromLittleEndian<quint16>(f + 36);
            quint32 color = defaultColor;
            if (materialise) {
                if (!(attr & 0x8000))
                    color = ModelPack::colorFromBytes(expand5(attr & 31), expand5((attr >> 5) & 31),
                                                      expand5((attr >> 10) & 31), 255);
            } else if (attr & 0x8000) {
                color = ModelPack::colorFromBytes(expand5((attr >> 10) & 31), expand5((attr >> 5) & 31),
                                                  expand5(attr & 31), 255);
                colored = true;
            }
            ModelVertex *v = out + qint64(t) * 3;
            for (int k = 0; k < 3; ++k) {
                v[k].px = lef32(f + k * 12);
                v[k].py = lef32(f + k * 12 + 4);
                v[k].pz = lef32(f + k * 12 + 8);
                v[k].color = color;
            }
        }
        if (colored)
            anyColor.store(true, std::memory_order_relaxed);
    });
    if (cancel.load(std::memory_order_relaxed))
        return ModelLoader::cancelled();

    ModelPrimitive prim;
    prim.vertexCount = quint32(doc->vertices.size());
    prim.flatNormals = true;
    prim.hasColor = anyColor.load();
    if (!prim.hasColor) {
        for (ModelVertex &v : doc->vertices)
            v.color = 0xFFFFFFFFu;
    }
    singleMesh(*doc, prim, QStringLiteral("STL"));
    Result r;
    r.doc = std::move(doc);
    return r;
}

// ---------------------------------------------------------------------------
// PLY

namespace {

enum class PlyType : quint8 { Invalid, I8, U8, I16, U16, I32, U32, F32, F64 };

PlyType plyType(QByteArrayView t)
{
    if (t == "char" || t == "int8") return PlyType::I8;
    if (t == "uchar" || t == "uint8") return PlyType::U8;
    if (t == "short" || t == "int16") return PlyType::I16;
    if (t == "ushort" || t == "uint16") return PlyType::U16;
    if (t == "int" || t == "int32") return PlyType::I32;
    if (t == "uint" || t == "uint32") return PlyType::U32;
    if (t == "float" || t == "float32") return PlyType::F32;
    if (t == "double" || t == "float64") return PlyType::F64;
    return PlyType::Invalid;
}

int plySize(PlyType t)
{
    switch (t) {
    case PlyType::I8: case PlyType::U8: return 1;
    case PlyType::I16: case PlyType::U16: return 2;
    case PlyType::I32: case PlyType::U32: case PlyType::F32: return 4;
    case PlyType::F64: return 8;
    case PlyType::Invalid: break;
    }
    return 0;
}

double plyRead(const char *p, PlyType t, bool be)
{
    switch (t) {
    case PlyType::I8: return double(static_cast<qint8>(*p));
    case PlyType::U8: return double(static_cast<quint8>(*p));
    case PlyType::I16: return double(be ? qFromBigEndian<qint16>(p) : qFromLittleEndian<qint16>(p));
    case PlyType::U16: return double(be ? qFromBigEndian<quint16>(p) : qFromLittleEndian<quint16>(p));
    case PlyType::I32: return double(be ? qFromBigEndian<qint32>(p) : qFromLittleEndian<qint32>(p));
    case PlyType::U32: return double(be ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p));
    case PlyType::F32: {
        const quint32 bits = be ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return double(f);
    }
    case PlyType::F64: {
        const quint64 bits = be ? qFromBigEndian<quint64>(p) : qFromLittleEndian<quint64>(p);
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        return d;
    }
    case PlyType::Invalid: break;
    }
    return 0.0;
}

struct PlyProperty {
    QByteArray name;
    PlyType type = PlyType::Invalid;
    bool list = false;
    PlyType countType = PlyType::Invalid;
};

struct PlyElement {
    QByteArray name;
    quint64 count = 0;
    std::vector<PlyProperty> props;
    bool hasList() const
    {
        return std::any_of(props.begin(), props.end(), [](const PlyProperty &p) { return p.list; });
    }
};

enum class PlyFormat : quint8 { Ascii, LittleEndian, BigEndian };

// Vertex property roles.
enum Role : std::uint8_t { X, Y, Z, NX, NY, NZ, R, G, B, A, U, V, RoleCount };

int roleFor(const QByteArray &name)
{
    static const QHash<QByteArray, int> roles = {
        {"x", X}, {"y", Y}, {"z", Z}, {"nx", NX}, {"ny", NY}, {"nz", NZ},
        {"red", R}, {"green", G}, {"blue", B}, {"alpha", A}, {"r", R}, {"g", G}, {"b", B}, {"a", A},
        {"diffuse_red", R}, {"diffuse_green", G}, {"diffuse_blue", B},
        {"s", U}, {"t", V}, {"u", U}, {"v", V}, {"texture_u", U}, {"texture_v", V},
        {"texture_s", U}, {"texture_t", V},
    };
    return roles.value(name, -1);
}

// Bounded cursor over the element data (binary or ASCII).
struct PlyCursor {
    const char *p;
    const char *end;
    PlyFormat format;
    bool ok = true;

    double scalar(PlyType t)
    {
        if (format == PlyFormat::Ascii) {
            double v = 0;
            if (!nextNumber(p, end, v))
                ok = false;
            return v;
        }
        const int n = plySize(t);
        if (end - p < n) {
            ok = false;
            p = end;
            return 0.0;
        }
        const double v = plyRead(p, t, format == PlyFormat::BigEndian);
        p += n;
        return v;
    }

    // Most items of `itemSize` bytes (binary) or numbers (ASCII: a digit and
    // a separator each, the last one may end the file) the rest can hold.
    quint64 itemsLeft(int itemSize) const
    {
        const quint64 left = quint64(end - p);
        return format == PlyFormat::Ascii ? (left + 1) / 2 : left / quint64(std::max(itemSize, 1));
    }

    // A list length: finite, non-negative and no more items than the rest of
    // the file can hold, so walking the list is bounded by the file size.
    bool listCount(PlyType countType, PlyType itemType, quint64 &n)
    {
        const double v = scalar(countType);
        if (!ok || !(v >= 0.0) || !(v <= double(itemsLeft(plySize(itemType))))) {
            ok = false;
            p = end;
            return false;
        }
        n = quint64(v);
        return true;
    }

    // Walks past n items of a list whose length listCount() accepted.
    void skipItems(PlyType t, quint64 n)
    {
        if (format == PlyFormat::Ascii) {
            for (quint64 k = 0; k < n && ok; ++k)
                scalar(t);
        } else {
            p += qint64(n) * plySize(t); // n <= itemsLeft(): stays in the file
        }
    }

    // Fewest bytes one row of `e` takes; 0 when it reads nothing.
    quint64 minRowBytes(const PlyElement &e) const
    {
        quint64 bytes = 0;
        for (const PlyProperty &prop : e.props)
            bytes += format == PlyFormat::Ascii ? 2 : quint64(plySize(prop.list ? prop.countType : prop.type));
        return bytes;
    }

    // Whether the rest of the file can hold e.count rows (checked before
    // anything is allocated from the header's count).
    bool canHold(const PlyElement &e) const
    {
        const quint64 row = minRowBytes(e);
        if (row == 0)
            return true;
        const quint64 left = quint64(end - p) + (format == PlyFormat::Ascii ? 1 : 0);
        return e.count <= left / row;
    }
};

} // namespace

Result loadPly(QByteArrayView bytes, const std::atomic_bool &cancel)
{
    // Header.
    qsizetype pos = 0;
    PlyFormat format = PlyFormat::Ascii;
    std::vector<PlyElement> elements;
    bool ended = false;
    bool sawFormat = false;
    while (pos < bytes.size()) {
        qsizetype nl = bytes.indexOf('\n', pos);
        if (nl < 0)
            nl = bytes.size();
        QByteArrayView line = bytes.sliced(pos, nl - pos);
        pos = nl + 1;
        if (line.endsWith('\r'))
            line.chop(1);
        const QList<QByteArray> words = line.toByteArray().simplified().split(' ');
        if (words.isEmpty() || words[0].isEmpty())
            continue;
        const QByteArray &key = words[0];
        if (key == "end_header") {
            ended = true;
            break;
        }
        if (key == "format" && words.size() >= 2) {
            sawFormat = true;
            if (words[1] == "ascii") format = PlyFormat::Ascii;
            else if (words[1] == "binary_little_endian") format = PlyFormat::LittleEndian;
            else if (words[1] == "binary_big_endian") format = PlyFormat::BigEndian;
            else return ModelLoader::failed(QObject::tr("Unknown PLY format \"%1\".").arg(QString::fromLatin1(words[1])));
        } else if (key == "element" && words.size() >= 3) {
            PlyElement e;
            e.name = words[1];
            e.count = words[2].toULongLong();
            elements.push_back(std::move(e));
        } else if (key == "property" && !elements.empty()) {
            PlyProperty prop;
            if (words.size() >= 5 && words[1] == "list") {
                prop.list = true;
                prop.countType = plyType(words[2]);
                prop.type = plyType(words[3]);
                prop.name = words[4];
            } else if (words.size() >= 3) {
                prop.type = plyType(words[1]);
                prop.name = words[2];
            }
            if (prop.type == PlyType::Invalid || (prop.list && prop.countType == PlyType::Invalid))
                return ModelLoader::failed(QObject::tr("Unsupported PLY property \"%1\".").arg(QString::fromLatin1(line.toByteArray())));
            elements.back().props.push_back(std::move(prop));
        }
    }
    if (!ended || !sawFormat)
        return ModelLoader::failed(QObject::tr("Not a valid PLY file."));

    auto doc = std::make_shared<ModelDocument>();
    doc->format = format == PlyFormat::Ascii ? QObject::tr("PLY ASCII") : QObject::tr("PLY binary");

    PlyCursor cur{bytes.data() + pos, bytes.data() + bytes.size(), format};
    bool hasNormals = false;
    bool hasColor = false;
    bool sawFaces = false;
    quint64 vertexCount = 0;

    for (const PlyElement &e : elements) {
        if (cancel.load(std::memory_order_relaxed))
            return ModelLoader::cancelled();
        if (e.name != "vertex" && cur.minRowBytes(e) == 0)
            continue; // rows without properties read nothing
        if (e.name == "vertex" && doc->vertices.empty()) {
            constexpr quint64 kVertexSize = sizeof(ModelVertex);
            if (e.count > quint64(kCap) / kVertexSize) {
                constexpr quint64 kMaxBytes = quint64(std::numeric_limits<qint64>::max());
                return ModelLoader::tooLarge(e.count > kMaxBytes / kVertexSize ? qint64(kMaxBytes)
                                                                               : qint64(e.count * kVertexSize));
            }
            if (e.count > 0 && cur.minRowBytes(e) == 0)
                return ModelLoader::failed(QObject::tr("The PLY vertex element has no properties."));
            if (!cur.canHold(e))
                return ModelLoader::failed(QObject::tr("The PLY file is truncated."));
            std::vector<int> role(e.props.size(), -1);
            std::array<bool, RoleCount> present{};
            std::array<bool, RoleCount> byteColor{};
            for (size_t i = 0; i < e.props.size(); ++i) {
                if (e.props[i].list)
                    continue;
                role[i] = roleFor(e.props[i].name);
                if (role[i] >= 0) {
                    present[size_t(role[i])] = true;
                    byteColor[size_t(role[i])] = e.props[i].type == PlyType::U8;
                }
            }
            hasNormals = present[NX] && present[NY] && present[NZ];
            hasColor = present[R] && present[G] && present[B];
            const bool hasUv = present[U] && present[V];
            doc->vertices.resize(size_t(e.count));
            vertexCount = e.count;

            auto store = [&](ModelVertex &v, const double *val) {
                v.px = float(val[X]);
                v.py = float(val[Y]);
                v.pz = float(val[Z]);
                if (hasNormals)
                    v.normal = ModelPack::normal(float(val[NX]), float(val[NY]), float(val[NZ]));
                if (hasUv) {
                    v.u = float(val[U]);
                    v.v = 1.0f - float(val[V]); // bottom-up
                }
                if (hasColor) {
                    auto comp = [&](int r) {
                        return byteColor[size_t(r)] ? quint8(std::clamp(val[r], 0.0, 255.0)) : ModelPack::unorm8(val[r]);
                    };
                    v.color = ModelPack::colorFromBytes(comp(R), comp(G), comp(B), present[A] ? comp(A) : 255);
                }
            };

            if (format != PlyFormat::Ascii && !e.hasList()) {
                // Fixed stride: decode in parallel straight from the mapping.
                std::vector<qint64> offset(e.props.size(), 0);
                qint64 stride = 0;
                for (size_t i = 0; i < e.props.size(); ++i) {
                    offset[i] = stride;
                    stride += plySize(e.props[i].type);
                }
                // canHold() checked count * stride against the rest of the file.
                const char *data = cur.p;
                const bool be = format == PlyFormat::BigEndian;
                ImageOps::parallelRows(int(e.count), qint64(e.count), [&](int r0, int r1) {
                    double val[RoleCount] = {};
                    for (int r = r0; r < r1; ++r) {
                        const char *row = data + qint64(r) * stride;
                        for (size_t i = 0; i < e.props.size(); ++i)
                            if (role[i] >= 0)
                                val[role[i]] = plyRead(row + offset[i], e.props[i].type, be);
                        store(doc->vertices[size_t(r)], val);
                    }
                });
                cur.p += stride * qint64(e.count);
            } else {
                double val[RoleCount] = {};
                for (quint64 r = 0; r < e.count && cur.ok; ++r) {
                    for (size_t i = 0; i < e.props.size(); ++i) {
                        const PlyProperty &prop = e.props[i];
                        if (prop.list) {
                            quint64 n = 0;
                            if (cur.listCount(prop.countType, prop.type, n))
                                cur.skipItems(prop.type, n);
                            continue;
                        }
                        const double v = cur.scalar(prop.type);
                        if (role[i] >= 0)
                            val[role[i]] = v;
                    }
                    store(doc->vertices[size_t(r)], val);
                    if ((r & 0xFFFF) == 0 && cancel.load(std::memory_order_relaxed))
                        return ModelLoader::cancelled();
                }
                if (!cur.ok)
                    return ModelLoader::failed(QObject::tr("The PLY file is truncated."));
            }
            continue;
        }

        if (e.name == "face" && !sawFaces) {
            sawFaces = true;
            int indexProp = -1;
            for (size_t i = 0; i < e.props.size(); ++i) {
                if (e.props[i].list && (e.props[i].name == "vertex_indices" || e.props[i].name == "vertex_index")) {
                    indexProp = int(i);
                    break;
                }
            }
            if (!cur.canHold(e))
                return ModelLoader::failed(QObject::tr("The PLY file is truncated."));
            // Room left under the cap for indices; checked per face, since one
            // face can add up to 196K of them.
            const qint64 vertexBytes = doc->geometryBytes();
            const quint64 indexBudget = vertexBytes < kCap ? quint64(kCap - vertexBytes) / sizeof(quint32) : 0;
            if (indexProp >= 0) {
                // Reserve for the triangles the rest of the file can hold:
                // each needs at least a full row with a 3-index list.
                const PlyProperty &ip = e.props[size_t(indexProp)];
                const quint64 triRow = cur.minRowBytes(e) + (format == PlyFormat::Ascii ? 6 : 3 * quint64(plySize(ip.type)));
                const quint64 left = quint64(cur.end - cur.p) + 1;
                doc->indices.reserve(size_t(std::min({e.count, left / triRow, indexBudget / 3}) * 3));
            }
            std::vector<quint32> poly;
            for (quint64 f = 0; f < e.count && cur.ok; ++f) {
                for (size_t i = 0; i < e.props.size(); ++i) {
                    const PlyProperty &prop = e.props[i];
                    if (!prop.list) {
                        cur.scalar(prop.type);
                        continue;
                    }
                    quint64 n = 0;
                    if (!cur.listCount(prop.countType, prop.type, n))
                        break;
                    if (int(i) != indexProp) {
                        cur.skipItems(prop.type, n);
                        continue;
                    }
                    poly.resize(size_t(std::min<quint64>(n, 1u << 16)));
                    for (quint64 k = 0; k < n && cur.ok; ++k) {
                        const double v = cur.scalar(prop.type);
                        if (k < poly.size()) // NaN, infinities and out-of-range indices read as vertex 0
                            poly[size_t(k)] = v >= 0.0 && v < double(vertexCount) ? quint32(v) : 0;
                    }
                    if (poly.size() < 3)
                        continue;
                    const size_t added = (poly.size() - 2) * 3;
                    if (doc->indices.size() + added > indexBudget)
                        return ModelLoader::tooLarge(vertexBytes
                                                     + qint64(doc->indices.size() + added) * qint64(sizeof(quint32)));
                    for (size_t k = 1; k + 1 < poly.size(); ++k) {
                        doc->indices.push_back(poly[0]);
                        doc->indices.push_back(poly[k]);
                        doc->indices.push_back(poly[k + 1]);
                    }
                }
                if ((f & 0xFFFF) == 0 && cancel.load(std::memory_order_relaxed))
                    return ModelLoader::cancelled();
            }
            continue;
        }

        // Any other element: walk past it.
        if (!cur.canHold(e))
            break;
        if (format != PlyFormat::Ascii && !e.hasList()) {
            cur.p += qint64(cur.minRowBytes(e)) * qint64(e.count); // canHold(): inside the file
            continue;
        }
        for (quint64 r = 0; r < e.count && cur.ok; ++r) {
            for (const PlyProperty &p : e.props) {
                quint64 n = 0;
                if (!p.list)
                    cur.scalar(p.type);
                else if (cur.listCount(p.countType, p.type, n))
                    cur.skipItems(p.type, n);
            }
            if ((r & 0xFFFF) == 0 && cancel.load(std::memory_order_relaxed))
                return ModelLoader::cancelled();
        }
    }

    if (doc->vertices.empty())
        return ModelLoader::failed(QObject::tr("The PLY file has no vertices."));
    ModelPrimitive prim;
    prim.hasColor = hasColor;
    if (doc->indices.empty()) {
        prim.topology = ModelTopology::Points; // point cloud (scans)
        prim.vertexCount = quint32(doc->vertices.size());
    } else {
        prim.indexCount = quint32(std::min<size_t>(doc->indices.size(), 0xFFFFFFFFu / 3 * 3));
        prim.vertexCount = quint32(doc->vertices.size());
        prim.flatNormals = !hasNormals;
    }
    singleMesh(*doc, prim, QStringLiteral("PLY"));
    Result r;
    r.doc = std::move(doc);
    return r;
}

// ---------------------------------------------------------------------------
// VOX (MagicaVoxel)

namespace {

// 0xAABBGGRR, index 0 unused: the same byte order as ModelVertex::color.
const quint32 kDefaultPalette[256] = {
    0x00000000u, 0xffffffffu, 0xffccffffu, 0xff99ffffu, 0xff66ffffu, 0xff33ffffu, 0xff00ffffu, 0xffffccffu,
    0xffccccffu, 0xff99ccffu, 0xff66ccffu, 0xff33ccffu, 0xff00ccffu, 0xffff99ffu, 0xffcc99ffu, 0xff9999ffu,
    0xff6699ffu, 0xff3399ffu, 0xff0099ffu, 0xffff66ffu, 0xffcc66ffu, 0xff9966ffu, 0xff6666ffu, 0xff3366ffu,
    0xff0066ffu, 0xffff33ffu, 0xffcc33ffu, 0xff9933ffu, 0xff6633ffu, 0xff3333ffu, 0xff0033ffu, 0xffff00ffu,
    0xffcc00ffu, 0xff9900ffu, 0xff6600ffu, 0xff3300ffu, 0xff0000ffu, 0xffffffccu, 0xffccffccu, 0xff99ffccu,
    0xff66ffccu, 0xff33ffccu, 0xff00ffccu, 0xffffccccu, 0xffccccccu, 0xff99ccccu, 0xff66ccccu, 0xff33ccccu,
    0xff00ccccu, 0xffff99ccu, 0xffcc99ccu, 0xff9999ccu, 0xff6699ccu, 0xff3399ccu, 0xff0099ccu, 0xffff66ccu,
    0xffcc66ccu, 0xff9966ccu, 0xff6666ccu, 0xff3366ccu, 0xff0066ccu, 0xffff33ccu, 0xffcc33ccu, 0xff9933ccu,
    0xff6633ccu, 0xff3333ccu, 0xff0033ccu, 0xffff00ccu, 0xffcc00ccu, 0xff9900ccu, 0xff6600ccu, 0xff3300ccu,
    0xff0000ccu, 0xffffff99u, 0xffccff99u, 0xff99ff99u, 0xff66ff99u, 0xff33ff99u, 0xff00ff99u, 0xffffcc99u,
    0xffcccc99u, 0xff99cc99u, 0xff66cc99u, 0xff33cc99u, 0xff00cc99u, 0xffff9999u, 0xffcc9999u, 0xff999999u,
    0xff669999u, 0xff339999u, 0xff009999u, 0xffff6699u, 0xffcc6699u, 0xff996699u, 0xff666699u, 0xff336699u,
    0xff006699u, 0xffff3399u, 0xffcc3399u, 0xff993399u, 0xff663399u, 0xff333399u, 0xff003399u, 0xffff0099u,
    0xffcc0099u, 0xff990099u, 0xff660099u, 0xff330099u, 0xff000099u, 0xffffff66u, 0xffccff66u, 0xff99ff66u,
    0xff66ff66u, 0xff33ff66u, 0xff00ff66u, 0xffffcc66u, 0xffcccc66u, 0xff99cc66u, 0xff66cc66u, 0xff33cc66u,
    0xff00cc66u, 0xffff9966u, 0xffcc9966u, 0xff999966u, 0xff669966u, 0xff339966u, 0xff009966u, 0xffff6666u,
    0xffcc6666u, 0xff996666u, 0xff666666u, 0xff336666u, 0xff006666u, 0xffff3366u, 0xffcc3366u, 0xff993366u,
    0xff663366u, 0xff333366u, 0xff003366u, 0xffff0066u, 0xffcc0066u, 0xff990066u, 0xff660066u, 0xff330066u,
    0xff000066u, 0xffffff33u, 0xffccff33u, 0xff99ff33u, 0xff66ff33u, 0xff33ff33u, 0xff00ff33u, 0xffffcc33u,
    0xffcccc33u, 0xff99cc33u, 0xff66cc33u, 0xff33cc33u, 0xff00cc33u, 0xffff9933u, 0xffcc9933u, 0xff999933u,
    0xff669933u, 0xff339933u, 0xff009933u, 0xffff6633u, 0xffcc6633u, 0xff996633u, 0xff666633u, 0xff336633u,
    0xff006633u, 0xffff3333u, 0xffcc3333u, 0xff993333u, 0xff663333u, 0xff333333u, 0xff003333u, 0xffff0033u,
    0xffcc0033u, 0xff990033u, 0xff660033u, 0xff330033u, 0xff000033u, 0xffffff00u, 0xffccff00u, 0xff99ff00u,
    0xff66ff00u, 0xff33ff00u, 0xff00ff00u, 0xffffcc00u, 0xffcccc00u, 0xff99cc00u, 0xff66cc00u, 0xff33cc00u,
    0xff00cc00u, 0xffff9900u, 0xffcc9900u, 0xff999900u, 0xff669900u, 0xff339900u, 0xff009900u, 0xffff6600u,
    0xffcc6600u, 0xff996600u, 0xff666600u, 0xff336600u, 0xff006600u, 0xffff3300u, 0xffcc3300u, 0xff993300u,
    0xff663300u, 0xff333300u, 0xff003300u, 0xffff0000u, 0xffcc0000u, 0xff990000u, 0xff660000u, 0xff330000u,
    0xff0000eeu, 0xff0000ddu, 0xff0000bbu, 0xff0000aau, 0xff000088u, 0xff000077u, 0xff000055u, 0xff000044u,
    0xff000022u, 0xff000011u, 0xff00ee00u, 0xff00dd00u, 0xff00bb00u, 0xff00aa00u, 0xff008800u, 0xff007700u,
    0xff005500u, 0xff004400u, 0xff002200u, 0xff001100u, 0xffee0000u, 0xffdd0000u, 0xffbb0000u, 0xffaa0000u,
    0xff880000u, 0xff770000u, 0xff550000u, 0xff440000u, 0xff220000u, 0xff110000u, 0xffeeeeeeu, 0xffddddddu,
    0xffbbbbbbu, 0xffaaaaaau, 0xff888888u, 0xff777777u, 0xff555555u, 0xff444444u, 0xff222222u, 0xff111111u,
};

constexpr int kMaxVoxSide = 512;
constexpr qint64 kMaxVoxGrid = qint64(kMaxVoxSide) * kMaxVoxSide * kMaxVoxSide;
// Scene graph: stack pushes and document nodes, each.
constexpr size_t kMaxVoxSceneNodes = size_t(1) << 20;

struct VoxReader {
    const char *p;
    const char *end;
    bool ok = true;

    bool has(qint64 n) const { return end - p >= n; }
    qint32 i32()
    {
        if (!has(4)) { ok = false; p = end; return 0; }
        const qint32 v = qFromLittleEndian<qint32>(p);
        p += 4;
        return v;
    }
    QByteArray string()
    {
        const qint32 n = i32();
        if (n < 0 || !has(n)) { ok = false; p = end; return QByteArray(); }
        QByteArray s(p, n);
        p += n;
        return s;
    }
    std::map<QByteArray, QByteArray> dict()
    {
        std::map<QByteArray, QByteArray> d;
        const qint32 n = i32();
        for (qint32 i = 0; i < n && ok; ++i) {
            QByteArray k = string();
            QByteArray v = string();
            d.emplace(std::move(k), std::move(v));
        }
        return d;
    }
};

struct VoxModel {
    int sx = 0, sy = 0, sz = 0;
    QByteArrayView xyzi; // 4 bytes per voxel, in the mapped file
};

struct VoxNode {
    enum class Kind : quint8 { Transform, Group, Shape } kind = Kind::Transform;
    QString name;
    bool hidden = false;
    bool visited = false; // the scene walk takes each node once
    int layer = -1;
    int child = -1;
    std::vector<int> children;
    std::vector<int> models;
    QMatrix4x4 local;
};

struct VoxMaterial {
    enum class Type : quint8 { Diffuse, Metal, Glass, Emit } type = Type::Diffuse;
    float rough = 0.8f;
    float metal = 0.0f;
    float alpha = 1.0f;
    float emission = 0.0f;
};

float dictFloat(const std::map<QByteArray, QByteArray> &d, const char *key, float def)
{
    const auto it = d.find(QByteArray(key));
    if (it == d.end())
        return def;
    bool ok = false;
    const float v = it->second.toFloat(&ok);
    return ok ? v : def;
}

QMatrix4x4 voxTransform(const std::map<QByteArray, QByteArray> &frame)
{
    QMatrix4x4 m;
    m.setToIdentity();
    const auto r = frame.find(QByteArray("_r"));
    if (r != frame.end()) {
        const int bits = r->second.toInt();
        const int i0 = bits & 3;
        const int i1 = (bits >> 2) & 3;
        const int i2 = 3 - i0 - i1;
        if (i0 <= 2 && i1 <= 2 && i0 != i1 && i2 >= 0 && i2 <= 2) {
            float rows[3][3] = {};
            rows[0][i0] = (bits >> 4) & 1 ? -1.0f : 1.0f;
            rows[1][i1] = (bits >> 5) & 1 ? -1.0f : 1.0f;
            rows[2][i2] = (bits >> 6) & 1 ? -1.0f : 1.0f;
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    m(row, col) = rows[row][col];
        }
    }
    const auto t = frame.find(QByteArray("_t"));
    if (t != frame.end()) {
        const QList<QByteArray> parts = t->second.simplified().split(' ');
        if (parts.size() == 3) {
            m(0, 3) = parts[0].toFloat();
            m(1, 3) = parts[1].toFloat();
            m(2, 3) = parts[2].toFloat();
        }
    }
    return m;
}

// Greedy meshing (Lysenko): one quad per maximal rectangle of equally
// coloured, equally lit faces in each slice. Appends to `doc`, one
// primitive per material class used. `geometryBytes` is the running size of
// the document's arrays; false (the model left unfinished) as soon as a quad
// would take it past the cap, so the refusal comes before the memory is spent.
bool meshModel(const VoxModel &model, const quint32 *palette, const std::vector<int> &materialOf, ModelDocument &doc,
               qint64 &geometryBytes)
{
    constexpr qint64 kQuadBytes = 4 * qint64(sizeof(ModelVertex)) + 6 * qint64(sizeof(quint32));
    if (model.xyzi.isEmpty()) {
        ModelMesh empty;
        empty.firstPrimitive = int(doc.primitives.size());
        doc.meshes.push_back(empty); // keeps mesh indices equal to model indices
        return true;
    }
    const int dims[3] = {model.sx, model.sy, model.sz};
    std::vector<quint8> grid(size_t(model.sx) * size_t(model.sy) * size_t(model.sz), 0);
    const auto *v = reinterpret_cast<const uchar *>(model.xyzi.data());
    for (qsizetype i = 0; i + 3 < model.xyzi.size(); i += 4) {
        const int x = v[i], y = v[i + 1], z = v[i + 2];
        if (x < model.sx && y < model.sy && z < model.sz)
            grid[size_t(x) + size_t(model.sx) * (size_t(y) + size_t(model.sy) * size_t(z))] = v[i + 3];
    }
    auto at = [&](int x, int y, int z) -> int {
        return grid[size_t(x) + size_t(model.sx) * (size_t(y) + size_t(model.sy) * size_t(z))];
    };
    // MagicaVoxel pivots a model about floor(size / 2).
    // NOLINTBEGIN(bugprone-integer-division): floor(size / 2) is the intended integer division
    const float pivot[3] = {float(model.sx / 2), float(model.sy / 2), float(model.sz / 2)};
    // NOLINTEND(bugprone-integer-division)

    // Indices per material slot, merged into primitives at the end.
    std::map<int, std::vector<quint32>> byMaterial;
    const quint32 base = quint32(doc.vertices.size());

    std::vector<int> mask;
    for (int d = 0; d < 3; ++d) {
        const int u = (d + 1) % 3;
        const int w = (d + 2) % 3;
        mask.assign(size_t(dims[u]) * size_t(dims[w]), 0);
        int x[3] = {0, 0, 0};
        int q[3] = {0, 0, 0};
        q[d] = 1;
        for (x[d] = -1; x[d] < dims[d];) {
            // Faces between slice x[d] and x[d] + 1: +colour faces +d, -colour faces -d.
            size_t n = 0;
            for (x[w] = 0; x[w] < dims[w]; ++x[w]) {
                for (x[u] = 0; x[u] < dims[u]; ++x[u], ++n) {
                    const int a = x[d] >= 0 ? at(x[0], x[1], x[2]) : 0;
                    const int b = x[d] < dims[d] - 1 ? at(x[0] + q[0], x[1] + q[1], x[2] + q[2]) : 0;
                    mask[n] = (a != 0) == (b != 0) ? 0 : a ? a : -b;
                }
            }
            ++x[d];
            n = 0;
            for (int j = 0; j < dims[w]; ++j) {
                for (int i = 0; i < dims[u];) {
                    const int c = mask[n];
                    if (!c) {
                        ++i;
                        ++n;
                        continue;
                    }
                    int width = 1;
                    while (i + width < dims[u] && mask[n + size_t(width)] == c)
                        ++width;
                    int height = 1;
                    for (; j + height < dims[w]; ++height) {
                        bool row = true;
                        for (int k = 0; k < width; ++k) {
                            if (mask[n + size_t(k) + size_t(height) * size_t(dims[u])] != c) {
                                row = false;
                                break;
                            }
                        }
                        if (!row)
                            break;
                    }
                    geometryBytes += kQuadBytes;
                    if (geometryBytes > kCap)
                        return false;
                    // Quad at plane x[d], spanning [i, i+width) x [j, j+height).
                    float p0[3];
                    p0[d] = float(x[d]);
                    p0[u] = float(i);
                    p0[w] = float(j);
                    float du[3] = {0, 0, 0};
                    float dw[3] = {0, 0, 0};
                    du[u] = float(width);
                    dw[w] = float(height);
                    const int index = c > 0 ? c : -c;
                    float nrm[3] = {0, 0, 0};
                    nrm[d] = c > 0 ? 1.0f : -1.0f;
                    const quint32 packedNormal = ModelPack::normal(nrm[0], nrm[1], nrm[2]);
                    const quint32 color = palette[index] | 0xFF000000u;
                    const quint32 first = quint32(doc.vertices.size()) - base;
                    for (int corner = 0; corner < 4; ++corner) {
                        ModelVertex vert;
                        const float su = (corner == 1 || corner == 2) ? 1.0f : 0.0f;
                        const float sw = (corner >= 2) ? 1.0f : 0.0f;
                        vert.px = p0[0] + du[0] * su + dw[0] * sw - pivot[0];
                        vert.py = p0[1] + du[1] * su + dw[1] * sw - pivot[1];
                        vert.pz = p0[2] + du[2] * su + dw[2] * sw - pivot[2];
                        vert.normal = packedNormal;
                        vert.color = color;
                        doc.vertices.push_back(vert);
                    }
                    std::vector<quint32> &out = byMaterial[materialOf[size_t(index)]];
                    if (c > 0) {
                        // CCW seen from +d: (0, u, u+w, w).
                        out.insert(out.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
                    } else {
                        out.insert(out.end(), {first, first + 2, first + 1, first, first + 3, first + 2});
                    }
                    for (int hh = 0; hh < height; ++hh)
                        for (int k = 0; k < width; ++k)
                            mask[n + size_t(k) + size_t(hh) * size_t(dims[u])] = 0;
                    i += width;
                    n += size_t(width);
                }
            }
        }
    }

    ModelMesh mesh;
    mesh.firstPrimitive = int(doc.primitives.size());
    const quint32 vertexCount = quint32(doc.vertices.size()) - base;
    for (auto &[material, indices] : byMaterial) {
        ModelPrimitive prim;
        prim.firstIndex = doc.indices.size();
        prim.indexCount = quint32(indices.size());
        prim.baseVertex = base;
        prim.vertexCount = vertexCount;
        prim.material = material;
        prim.hasColor = true;
        doc.indices.insert(doc.indices.end(), indices.begin(), indices.end());
        doc.primitives.push_back(prim);
    }
    mesh.primitiveCount = int(doc.primitives.size()) - mesh.firstPrimitive;
    doc.meshes.push_back(mesh);
    return true;
}

} // namespace

Result loadVox(QByteArrayView bytes, const std::atomic_bool &cancel)
{
    if (!ModelLoader::looksLikeVox(bytes))
        return ModelLoader::failed(QObject::tr("Not a MagicaVoxel file."));
    VoxReader rd{bytes.data() + 8, bytes.data() + bytes.size()};
    // MAIN chunk header.
    rd.p += 4;
    const qint32 mainContent = rd.i32();
    const qint32 mainChildren = rd.i32();
    if (!rd.ok || mainContent < 0 || mainChildren < 0 || !rd.has(qint64(mainContent) + mainChildren))
        return ModelLoader::failed(QObject::tr("The VOX file is truncated."));
    rd.p += mainContent;
    rd.end = rd.p + mainChildren;

    quint32 palette[256];
    std::memcpy(palette, kDefaultPalette, sizeof(palette));
    std::vector<VoxModel> models;
    std::map<int, VoxNode> nodes;
    std::map<int, bool> hiddenLayers;
    std::vector<VoxMaterial> materials(256);
    int pendingX = 0, pendingY = 0, pendingZ = 0;
    bool havePending = false;

    while (rd.ok && rd.has(12)) {
        const QByteArrayView id(rd.p, 4);
        rd.p += 4;
        const qint32 content = rd.i32();
        const qint32 children = rd.i32();
        if (content < 0 || children < 0 || !rd.has(qint64(content) + children))
            break;
        VoxReader c{rd.p, rd.p + content};
        rd.p += qint64(content) + children;

        if (id == "SIZE") {
            pendingX = c.i32();
            pendingY = c.i32();
            pendingZ = c.i32();
            havePending = c.ok;
        } else if (id == "XYZI" && havePending) {
            const qint32 count = c.i32();
            if (count < 0 || !c.has(qint64(count) * 4) || pendingX <= 0 || pendingY <= 0 || pendingZ <= 0)
                continue;
            if (pendingX > kMaxVoxSide || pendingY > kMaxVoxSide || pendingZ > kMaxVoxSide
                || qint64(pendingX) * pendingY * pendingZ > kMaxVoxGrid)
                return ModelLoader::failed(QObject::tr("A VOX model is larger than %1 voxels per side.").arg(kMaxVoxSide));
            VoxModel m;
            m.sx = pendingX;
            m.sy = pendingY;
            m.sz = pendingZ;
            m.xyzi = QByteArrayView(c.p, qint64(count) * 4);
            models.push_back(m);
            havePending = false;
        } else if (id == "RGBA") {
            for (int i = 0; i < 255 && c.has(4); ++i) {
                palette[i + 1] = qFromLittleEndian<quint32>(c.p); // R G B A bytes
                c.p += 4;
            }
        } else if (id == "nTRN") {
            const int nid = c.i32();
            const auto attrs = c.dict();
            VoxNode n;
            n.kind = VoxNode::Kind::Transform;
            if (auto it = attrs.find(QByteArray("_name")); it != attrs.end())
                n.name = QString::fromUtf8(it->second);
            if (auto it = attrs.find(QByteArray("_hidden")); it != attrs.end())
                n.hidden = it->second == "1";
            n.child = c.i32();
            c.i32(); // reserved
            n.layer = c.i32();
            const qint32 frames = c.i32();
            for (qint32 f = 0; f < frames && c.ok; ++f) {
                const auto frame = c.dict();
                if (f == 0)
                    n.local = voxTransform(frame);
            }
            if (c.ok)
                nodes[nid] = std::move(n);
        } else if (id == "nGRP") {
            const int nid = c.i32();
            c.dict();
            VoxNode n;
            n.kind = VoxNode::Kind::Group;
            const qint32 count = c.i32();
            for (qint32 i = 0; i < count && c.ok; ++i)
                n.children.push_back(c.i32());
            if (c.ok)
                nodes[nid] = std::move(n);
        } else if (id == "nSHP") {
            const int nid = c.i32();
            c.dict();
            VoxNode n;
            n.kind = VoxNode::Kind::Shape;
            const qint32 count = c.i32();
            for (qint32 i = 0; i < count && c.ok; ++i) {
                n.models.push_back(c.i32());
                c.dict();
            }
            if (c.ok)
                nodes[nid] = std::move(n);
        } else if (id == "MATL") {
            const int mid = c.i32();
            const auto attrs = c.dict();
            if (mid <= 0 || mid > 255 || !c.ok)
                continue;
            VoxMaterial &m = materials[size_t(mid)];
            const auto type = attrs.find(QByteArray("_type"));
            const QByteArray t = type != attrs.end() ? type->second : QByteArray();
            m.rough = dictFloat(attrs, "_rough", 0.8f);
            if (t == "_metal") {
                m.type = VoxMaterial::Type::Metal;
                m.metal = dictFloat(attrs, "_metal", 1.0f);
            } else if (t == "_glass" || t == "_blend") {
                m.type = VoxMaterial::Type::Glass;
                m.alpha = 1.0f - std::clamp(dictFloat(attrs, "_trans", dictFloat(attrs, "_alpha", 0.5f)), 0.0f, 0.95f);
            } else if (t == "_emit") {
                m.type = VoxMaterial::Type::Emit;
                m.emission = dictFloat(attrs, "_emit", 1.0f) * (1.0f + dictFloat(attrs, "_flux", 0.0f));
            }
        } else if (id == "LAYR") {
            const int lid = c.i32();
            const auto attrs = c.dict();
            if (auto it = attrs.find(QByteArray("_hidden")); it != attrs.end() && it->second == "1")
                hiddenLayers[lid] = true;
        }
    }
    if (models.empty())
        return ModelLoader::failed(QObject::tr("The VOX file has no models."));

    auto doc = std::make_shared<ModelDocument>();
    doc->format = QObject::tr("MagicaVoxel VOX");
    doc->zUp = true;

    // Material slot per palette index: 0 is the shared diffuse material;
    // every non-diffuse index gets its own (its colour drives emission).
    std::vector<int> materialOf(256, 0);
    ModelMaterial voxel;
    voxel.name = QObject::tr("Voxel");
    voxel.roughness = 0.85f;
    doc->materials.push_back(voxel);
    for (int i = 1; i < 256; ++i) {
        const VoxMaterial &m = materials[size_t(i)];
        if (m.type == VoxMaterial::Type::Diffuse)
            continue;
        ModelMaterial out;
        out.name = QObject::tr("Palette %1").arg(i);
        out.roughness = std::clamp(m.rough, 0.02f, 1.0f);
        if (m.type == VoxMaterial::Type::Metal) {
            out.metallic = std::clamp(m.metal, 0.0f, 1.0f);
        } else if (m.type == VoxMaterial::Type::Glass) {
            out.baseColor[3] = m.alpha;
            out.alphaMode = ModelAlphaMode::Blend;
        } else {
            const quint32 c = palette[i];
            out.emissive = {ModelPack::srgbToLinear(float(c & 0xFF) / 255.0f) * m.emission,
                            ModelPack::srgbToLinear(float((c >> 8) & 0xFF) / 255.0f) * m.emission,
                            ModelPack::srgbToLinear(float((c >> 16) & 0xFF) / 255.0f) * m.emission};
        }
        materialOf[size_t(i)] = int(doc->materials.size());
        doc->materials.push_back(out);
    }

    qint64 geometryBytes = 0;
    for (const VoxModel &m : models) {
        if (cancel.load(std::memory_order_relaxed))
            return ModelLoader::cancelled();
        if (!meshModel(m, palette, materialOf, *doc, geometryBytes))
            return ModelLoader::tooLarge(geometryBytes);
    }

    // Scene graph from node 0; without one, every model at the origin. Each
    // node is taken once (a group listing itself or a cycle adds nothing), and
    // pushes and document nodes are capped.
    if (nodes.count(0)) {
        std::vector<std::pair<int, int>> stack{{0, -1}};
        size_t pushes = 1;
        bool truncated = false;
        auto push = [&](int id, int parent) {
            if (pushes >= kMaxVoxSceneNodes) {
                truncated = true;
                return;
            }
            ++pushes;
            stack.emplace_back(id, parent);
        };
        while (!stack.empty()) {
            const auto [nid, parent] = stack.back();
            stack.pop_back();
            const auto it = nodes.find(nid);
            if (it == nodes.end() || it->second.visited)
                continue;
            VoxNode &n = it->second;
            n.visited = true;
            switch (n.kind) {
            case VoxNode::Kind::Transform: {
                if (n.hidden || (n.layer >= 0 && hiddenLayers.count(n.layer)))
                    break;
                if (doc->nodes.size() >= kMaxVoxSceneNodes) {
                    truncated = true;
                    break;
                }
                ModelNode node;
                node.name = n.name;
                node.parent = parent;
                node.hasMatrix = true;
                node.matrix = n.local;
                const int self = int(doc->nodes.size());
                doc->nodes.push_back(node);
                push(n.child, self);
                break;
            }
            case VoxNode::Kind::Group:
                for (auto c = n.children.rbegin(); c != n.children.rend(); ++c)
                    push(*c, parent);
                break;
            case VoxNode::Kind::Shape:
                for (int model : n.models) {
                    if (model < 0 || model >= int(doc->meshes.size()))
                        continue;
                    if (doc->nodes.size() >= kMaxVoxSceneNodes) {
                        truncated = true;
                        break;
                    }
                    ModelNode node;
                    node.parent = parent;
                    node.mesh = model;
                    doc->nodes.push_back(node);
                }
                break;
            }
        }
        if (truncated)
            doc->warnings << QObject::tr("the scene graph is too large; only part of it is shown");
    }
    if (std::none_of(doc->nodes.begin(), doc->nodes.end(), [](const ModelNode &n) { return n.mesh >= 0; })) {
        doc->nodes.clear();
        for (int i = 0; i < int(doc->meshes.size()); ++i) {
            ModelNode node;
            node.mesh = i;
            doc->nodes.push_back(node);
        }
    }
    Result r;
    r.doc = std::move(doc);
    return r;
}

} // namespace SimpleMeshLoaders
