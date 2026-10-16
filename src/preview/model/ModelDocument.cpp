#include "ModelDocument.h"

#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <limits>

qint64 ModelDocument::geometryBytes() const
{
    return qint64(vertices.size()) * qint64(sizeof(ModelVertex)) + qint64(skin.size()) * qint64(sizeof(ModelSkinVertex))
           + qint64(indices.size()) * qint64(sizeof(quint32));
}

qint64 ModelDocument::textureBytes() const
{
    qint64 total = 0;
    for (const ModelTexture &t : textures)
        total += t.image.sizeInBytes();
    return total;
}

namespace ModelPack {

namespace {

inline quint32 snorm10(float v)
{
    const float c = std::clamp(v, -1.0f, 1.0f);
    const int i = int(std::lround(c * 511.0f));
    return quint32(i) & 0x3FFu;
}

inline float linearToSrgb(float c)
{
    if (!(c > 0.0f)) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

} // namespace

quint32 normal(float x, float y, float z)
{
    const float len2 = x * x + y * y + z * z;
    if (len2 > 0.0f && std::isfinite(len2)) {
        const float inv = 1.0f / std::sqrt(len2);
        x *= inv;
        y *= inv;
        z *= inv;
    } else {
        x = 0.0f;
        y = 1.0f;
        z = 0.0f;
    }
    return snorm10(x) | (snorm10(y) << 10) | (snorm10(z) << 20);
}

quint32 colorFromLinear(float r, float g, float b, float a)
{
    return colorFromBytes(unorm8(linearToSrgb(r)), unorm8(linearToSrgb(g)), unorm8(linearToSrgb(b)), unorm8(a));
}

float srgbToLinear(float c)
{
    if (!(c > 0.0f)) return 0.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

ModelSkinVertex skinVertex(const quint32 *joints, const float *weights, int count)
{
    // Keep the four largest positive weights.
    std::array<std::pair<float, quint32>, 4> best{};
    int kept = 0;
    for (int i = 0; i < count; ++i) {
        const float w = weights[i];
        if (!(w > 0.0f))
            continue;
        if (kept < 4) {
            best[size_t(kept++)] = {w, joints[i]};
            continue;
        }
        auto smallest = std::min_element(best.begin(), best.end(),
                                         [](const auto &a, const auto &b) { return a.first < b.first; });
        if (w > smallest->first)
            *smallest = {w, joints[i]};
    }

    ModelSkinVertex out;
    float sum = 0.0f;
    for (int i = 0; i < kept; ++i)
        sum += best[size_t(i)].first;
    if (kept == 0 || !(sum > 0.0f)) {
        out.weights[0] = 255; // bound to joint 0, as glTF does for all-zero weights
        return out;
    }
    int total = 0;
    int largest = 0;
    for (int i = 0; i < kept; ++i) {
        out.joints[i] = quint16(std::min<quint32>(best[size_t(i)].second, 0xFFFFu));
        const int q = int(std::lround(best[size_t(i)].first / sum * 255.0f));
        out.weights[i] = quint8(std::clamp(q, 0, 255));
        total += out.weights[i];
        if (out.weights[i] > out.weights[largest])
            largest = i;
    }
    // Rounding drift goes to the largest weight so the bytes sum to 255.
    out.weights[largest] = quint8(std::clamp(int(out.weights[largest]) + 255 - total, 0, 255));
    return out;
}

} // namespace ModelPack

namespace ModelScene {

QMatrix4x4 localMatrix(const ModelNode &node)
{
    if (node.hasMatrix)
        return node.matrix;
    QMatrix4x4 m;
    m.translate(node.translation);
    m.rotate(node.rotation);
    m.scale(node.scale);
    return m;
}

std::vector<QMatrix4x4> restWorld(const ModelDocument &doc, const QMatrix4x4 &root)
{
    std::vector<QMatrix4x4> world(doc.nodes.size());
    for (size_t i = 0; i < doc.nodes.size(); ++i) {
        const ModelNode &n = doc.nodes[i];
        const QMatrix4x4 local = localMatrix(n);
        world[i] = (n.parent >= 0 ? world[size_t(n.parent)] : root) * local;
    }
    return world;
}

QMatrix4x4 rootTransform(bool zUp)
{
    QMatrix4x4 m;
    if (zUp)
        m.rotate(-90.0f, 1.0f, 0.0f, 0.0f);
    return m;
}

namespace {

constexpr quint64 kChunk = quint64(1) << 20; // elements per bounds job

struct BoundsJob {
    size_t primitive = 0;
    quint64 begin = 0;
    quint64 end = 0;
    QVector3D min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    QVector3D max{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
};

inline void grow(BoundsJob &job, const ModelVertex &v)
{
    job.min.setX(std::min(job.min.x(), v.px));
    job.min.setY(std::min(job.min.y(), v.py));
    job.min.setZ(std::min(job.min.z(), v.pz));
    job.max.setX(std::max(job.max.x(), v.px));
    job.max.setY(std::max(job.max.y(), v.py));
    job.max.setZ(std::max(job.max.z(), v.pz));
}

} // namespace

void finalize(ModelDocument &doc)
{
    // Bounds: split every primitive into jobs of at most kChunk elements so
    // one huge mesh still spreads across the pool, then reduce per primitive.
    std::vector<BoundsJob> jobs;
    for (size_t p = 0; p < doc.primitives.size(); ++p) {
        const ModelPrimitive &prim = doc.primitives[p];
        const quint64 count = prim.indexCount ? prim.indexCount : prim.vertexCount;
        for (quint64 b = 0; b < count; b += kChunk) {
            BoundsJob job;
            job.primitive = p;
            job.begin = b;
            job.end = std::min(count, b + kChunk);
            jobs.push_back(job);
        }
    }
    const ModelVertex *verts = doc.vertices.data();
    const size_t vertexCount = doc.vertices.size();
    const quint32 *idx = doc.indices.data();
    const std::vector<ModelPrimitive> &prims = doc.primitives;
    auto run = [&](BoundsJob &job) {
        const ModelPrimitive &prim = prims[job.primitive];
        if (prim.indexCount) {
            const quint32 *p = idx + prim.firstIndex;
            for (quint64 i = job.begin; i < job.end; ++i) {
                const size_t v = size_t(p[i]) + prim.baseVertex;
                if (v < vertexCount)
                    grow(job, verts[v]);
            }
        } else {
            for (quint64 i = job.begin; i < job.end; ++i) {
                const size_t v = size_t(prim.baseVertex) + size_t(i);
                if (v < vertexCount)
                    grow(job, verts[v]);
            }
        }
    };
    if (jobs.size() > 1)
        QtConcurrent::blockingMap(jobs, run);
    else
        for (BoundsJob &job : jobs)
            run(job);

    for (ModelPrimitive &prim : doc.primitives) {
        prim.boundsMin = QVector3D();
        prim.boundsMax = QVector3D();
    }
    std::vector<bool> seen(doc.primitives.size(), false);
    for (const BoundsJob &job : jobs) {
        if (job.min.x() > job.max.x())
            continue;
        ModelPrimitive &prim = doc.primitives[job.primitive];
        if (!seen[job.primitive]) {
            prim.boundsMin = job.min;
            prim.boundsMax = job.max;
            seen[job.primitive] = true;
            continue;
        }
        prim.boundsMin = QVector3D(std::min(prim.boundsMin.x(), job.min.x()), std::min(prim.boundsMin.y(), job.min.y()),
                                   std::min(prim.boundsMin.z(), job.min.z()));
        prim.boundsMax = QVector3D(std::max(prim.boundsMax.x(), job.max.x()), std::max(prim.boundsMax.y(), job.max.y()),
                                   std::max(prim.boundsMax.z(), job.max.z()));
    }

    ModelStats stats;
    for (const ModelPrimitive &prim : doc.primitives) {
        const quint64 count = prim.indexCount ? prim.indexCount : prim.vertexCount;
        switch (prim.topology) {
        case ModelTopology::Triangles: stats.triangles += count / 3; break;
        case ModelTopology::Lines: stats.lines += count / 2; break;
        case ModelTopology::Points: stats.points += count; break;
        }
    }
    stats.vertices = doc.vertices.size();
    stats.meshes = int(doc.meshes.size());
    for (const ModelSkin &s : doc.skins)
        stats.joints += int(s.joints.size());
    doc.stats = stats;
}

bool sceneBounds(const ModelDocument &doc, const std::vector<QMatrix4x4> &world, const QMatrix4x4 &root,
                 QVector3D &min, QVector3D &max)
{
    bool any = false;
    auto add = [&](const QVector3D &p) {
        if (!any) {
            min = max = p;
            any = true;
            return;
        }
        min = QVector3D(std::min(min.x(), p.x()), std::min(min.y(), p.y()), std::min(min.z(), p.z()));
        max = QVector3D(std::max(max.x(), p.x()), std::max(max.y(), p.y()), std::max(max.z(), p.z()));
    };
    for (size_t n = 0; n < doc.nodes.size() && n < world.size(); ++n) {
        const ModelNode &node = doc.nodes[n];
        if (node.mesh < 0 || node.mesh >= int(doc.meshes.size()) || !node.visible)
            continue;
        const ModelMesh &mesh = doc.meshes[size_t(node.mesh)];
        for (int p = mesh.firstPrimitive; p < mesh.firstPrimitive + mesh.primitiveCount; ++p) {
            const ModelPrimitive &prim = doc.primitives[size_t(p)];
            if (prim.boundsMin.x() > prim.boundsMax.x())
                continue;
            const QMatrix4x4 &m = prim.skin >= 0 ? root : world[n];
            for (int c = 0; c < 8; ++c) {
                const QVector3D corner((c & 1) ? prim.boundsMax.x() : prim.boundsMin.x(),
                                       (c & 2) ? prim.boundsMax.y() : prim.boundsMin.y(),
                                       (c & 4) ? prim.boundsMax.z() : prim.boundsMin.z());
                add(m.map(corner));
            }
        }
    }
    return any;
}

} // namespace ModelScene
