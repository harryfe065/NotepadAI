#include "ObjLoader.h"

#include <fast_obj.h>

#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace ObjLoader {

namespace {

constexpr quint32 kNone = std::numeric_limits<quint32>::max();

struct Io {
    const std::atomic_bool *cancel = nullptr;
};

void *fileOpen(const char *path, void *)
{
    auto file = std::make_unique<QFile>(QString::fromUtf8(path));
    if (!file->open(QIODevice::ReadOnly))
        return nullptr;
    return file.release();
}

void fileClose(void *file, void *)
{
    delete static_cast<QFile *>(file);
}

size_t fileRead(void *file, void *dst, size_t bytes, void *user)
{
    // A cancelled load reads as end of file; the result is dropped.
    if (static_cast<Io *>(user)->cancel->load(std::memory_order_relaxed))
        return 0;
    const qint64 n = static_cast<QFile *>(file)->read(static_cast<char *>(dst), qint64(bytes));
    return n > 0 ? size_t(n) : 0;
}

unsigned long fileSize(void *file, void *)
{
    return static_cast<unsigned long>(static_cast<QFile *>(file)->size());
}

// Normal maps are mostly blue (+Z); a bump slot holding a height map is
// grey. Sampled, so the check costs the same for any size.
bool looksLikeNormalMap(const QImage &image)
{
    if (image.isNull() || image.format() != QImage::Format_RGBA8888)
        return false;
    double r = 0, g = 0, b = 0;
    int n = 0;
    for (int sy = 0; sy < 16; ++sy) {
        const int y = (image.height() - 1) * sy / 15;
        const uchar *row = image.constScanLine(y);
        for (int sx = 0; sx < 16; ++sx) {
            const int x = (image.width() - 1) * sx / 15;
            r += row[x * 4];
            g += row[x * 4 + 1];
            b += row[x * 4 + 2];
            ++n;
        }
    }
    r /= n * 255.0;
    g /= n * 255.0;
    b /= n * 255.0;
    return b > 0.65 && std::abs(r - 0.5) < 0.2 && std::abs(g - 0.5) < 0.2;
}

ModelMaterial material(const fastObjMesh &mesh, const fastObjMaterial &m, ModelDocument &doc,
                       ModelLoader::TextureSet &textures)
{
    ModelMaterial out;
    out.name = m.name ? QString::fromUtf8(m.name) : QString();
    for (int i = 0; i < 3; ++i) {
        out.baseColor[size_t(i)] = ModelPack::srgbToLinear(m.Kd[i]);
        out.emissive[size_t(i)] = ModelPack::srgbToLinear(m.Ke[i]);
    }
    out.baseColor[3] = std::clamp(m.d, 0.0f, 1.0f);
    // Blinn-Phong exponent to roughness.
    out.roughness = std::clamp(std::sqrt(2.0f / (std::max(m.Ns, 0.0f) + 2.0f)), 0.04f, 1.0f);
    out.metallic = 0.0f;

    auto tex = [&](unsigned int index, bool srgb) {
        if (index == 0 || index >= mesh.texture_count)
            return -1;
        const fastObjTexture &t = mesh.textures[index];
        const QString path = t.path ? QString::fromUtf8(t.path) : QString();
        const QString name = t.name ? QString::fromUtf8(t.name) : QString();
        int i = path.isEmpty() ? -1 : textures.fromFile(doc, path, srgb);
        if (i < 0 && !name.isEmpty())
            i = textures.fromFile(doc, name, srgb);
        return i;
    };
    out.baseColorTexture = tex(m.map_Kd, true);
    out.emissiveTexture = tex(m.map_Ke, true);
    if (out.emissiveTexture >= 0 && out.emissive == std::array<float, 3>{0, 0, 0})
        out.emissive = {1, 1, 1};
    out.normalTexture = tex(m.map_bump, false); // kept only if it is a normal map (checked after decode)
    if (m.map_d && m.map_d == m.map_Kd)
        out.alphaMode = ModelAlphaMode::Mask;   // cut-out in the colour map's alpha
    else if (out.baseColor[3] < 0.999f)
        out.alphaMode = ModelAlphaMode::Blend;
    return out;
}

} // namespace

ModelLoader::Result load(const QString &path, const std::atomic_bool &cancel)
{
    Io io;
    io.cancel = &cancel;
    fastObjCallbacks callbacks;
    callbacks.file_open = fileOpen;
    callbacks.file_close = fileClose;
    callbacks.file_read = fileRead;
    callbacks.file_size = fileSize;

    const QByteArray pathUtf8 = QFileInfo(path).absoluteFilePath().toUtf8();
    std::unique_ptr<fastObjMesh, decltype(&fast_obj_destroy)> mesh(
        fast_obj_read_with_callbacks(pathUtf8.constData(), &callbacks, &io), &fast_obj_destroy);
    if (cancel.load(std::memory_order_relaxed))
        return ModelLoader::cancelled();
    if (!mesh)
        return ModelLoader::failed(QObject::tr("Could not read the OBJ file."));

    auto doc = std::make_shared<ModelDocument>();
    doc->format = QStringLiteral("Wavefront OBJ");
    ModelLoader::TextureSet textures(path);

    // Materials; faces without one use slot `materialCount` (default).
    const unsigned materialCount = mesh->material_count;
    for (unsigned i = 0; i < materialCount; ++i)
        doc->materials.push_back(material(*mesh, mesh->materials[i], *doc, textures));

    // Index counts per material, in face order (fan triangulation).
    std::vector<quint64> perMaterial(materialCount + 1, 0);
    quint64 corners = 0;
    for (unsigned f = 0; f < mesh->face_count; ++f) {
        const unsigned fv = mesh->face_vertices[f];
        const unsigned m = mesh->face_materials ? std::min(mesh->face_materials[f], materialCount) : materialCount;
        if (fv >= 3)
            perMaterial[m] += quint64(fv - 2) * 3;
        corners += fv;
    }
    quint64 indexTotal = 0;
    for (quint64 c : perMaterial)
        indexTotal += c;
    // Welded vertices are at most the corner count.
    const quint64 bound = corners * sizeof(ModelVertex) + indexTotal * sizeof(quint32);
    if (bound > quint64(ModelLoader::kMaxGeometryBytes))
        return ModelLoader::tooLarge(qint64(bound));
    if (indexTotal == 0)
        return ModelLoader::failed(QObject::tr("The OBJ file has no faces."));

    std::vector<quint64> cursor(materialCount + 1, 0);
    for (unsigned m = 1; m <= materialCount; ++m)
        cursor[m] = cursor[m - 1] + perMaterial[m - 1];
    const std::vector<quint64> first = cursor;
    doc->indices.resize(size_t(indexTotal));

    const bool fileHasNormals = mesh->normal_count > 1; // slot 0 is fast_obj's dummy
    const bool hasColors = mesh->color_count > 1 && mesh->colors;
    std::vector<bool> flat(materialCount + 1, !fileHasNormals);

    // Weld (p, t, n) corners: a chain of vertices per position.
    std::vector<quint32> head(mesh->position_count, kNone);
    std::vector<quint32> next;
    std::vector<quint32> keyT;
    std::vector<quint32> keyN;
    doc->vertices.reserve(mesh->position_count);
    next.reserve(mesh->position_count);
    keyT.reserve(mesh->position_count);
    keyN.reserve(mesh->position_count);

    auto vertexFor = [&](const fastObjIndex &ix) -> quint32 {
        const quint32 p = ix.p < mesh->position_count ? ix.p : 0;
        const quint32 t = ix.t < mesh->texcoord_count ? ix.t : 0;
        const quint32 n = ix.n < mesh->normal_count ? ix.n : 0;
        for (quint32 v = head[p]; v != kNone; v = next[v])
            if (keyT[v] == t && keyN[v] == n)
                return v;
        const quint32 v = quint32(doc->vertices.size());
        next.push_back(head[p]);
        head[p] = v;
        keyT.push_back(t);
        keyN.push_back(n);
        ModelVertex out;
        out.px = mesh->positions[size_t(p) * 3];
        out.py = mesh->positions[size_t(p) * 3 + 1];
        out.pz = mesh->positions[size_t(p) * 3 + 2];
        if (t) {
            out.u = mesh->texcoords[size_t(t) * 2];
            out.v = 1.0f - mesh->texcoords[size_t(t) * 2 + 1]; // OBJ UVs are bottom-up
        }
        if (n)
            out.normal = ModelPack::normal(mesh->normals[size_t(n) * 3], mesh->normals[size_t(n) * 3 + 1],
                                           mesh->normals[size_t(n) * 3 + 2]);
        if (hasColors && p < mesh->color_count)
            out.color = ModelPack::colorFromLinear(mesh->colors[size_t(p) * 3], mesh->colors[size_t(p) * 3 + 1],
                                                   mesh->colors[size_t(p) * 3 + 2], 1.0f);
        doc->vertices.push_back(out);
        return v;
    };

    quint64 corner = 0;
    std::vector<quint32> faceVerts;
    for (unsigned f = 0; f < mesh->face_count; ++f) {
        if ((f & 0xFFFF) == 0 && cancel.load(std::memory_order_relaxed))
            return ModelLoader::cancelled();
        const unsigned fv = mesh->face_vertices[f];
        const unsigned m = mesh->face_materials ? std::min(mesh->face_materials[f], materialCount) : materialCount;
        faceVerts.resize(fv);
        for (unsigned k = 0; k < fv; ++k) {
            const fastObjIndex &ix = mesh->indices[corner + k];
            if (fileHasNormals && ix.n == 0)
                flat[m] = true;
            faceVerts[k] = vertexFor(ix);
        }
        corner += fv;
        if (fv < 3)
            continue;
        quint32 *out = doc->indices.data() + cursor[m];
        for (unsigned k = 1; k + 1 < fv; ++k) {
            *out++ = faceVerts[0];
            *out++ = faceVerts[k];
            *out++ = faceVerts[k + 1];
        }
        cursor[m] += quint64(fv - 2) * 3;
    }
    doc->vertices.shrink_to_fit();

    ModelMesh out;
    out.name = QFileInfo(path).completeBaseName();
    for (unsigned m = 0; m <= materialCount; ++m) {
        if (!perMaterial[m])
            continue;
        ModelPrimitive prim;
        prim.firstIndex = first[m];
        prim.indexCount = quint32(std::min<quint64>(perMaterial[m], std::numeric_limits<quint32>::max() / 3 * 3));
        prim.vertexCount = quint32(doc->vertices.size());
        prim.material = m < materialCount ? int(m) : -1;
        prim.flatNormals = flat[m];
        prim.hasColor = hasColors;
        doc->primitives.push_back(prim);
    }
    out.primitiveCount = int(doc->primitives.size());
    doc->meshes.push_back(out);
    ModelNode node;
    node.name = out.name;
    node.mesh = 0;
    doc->nodes.push_back(node);

    textures.decode(*doc, cancel);
    for (ModelMaterial &m : doc->materials) {
        if (m.normalTexture >= 0 && !looksLikeNormalMap(doc->textures[size_t(m.normalTexture)].image))
            m.normalTexture = -1; // a bump (height) map, which the viewer does not use
    }
    ModelLoader::Result r;
    r.doc = std::move(doc);
    return r;
}

} // namespace ObjLoader
