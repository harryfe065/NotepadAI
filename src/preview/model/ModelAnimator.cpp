#include "ModelAnimator.h"

#include <algorithm>
#include <cmath>

namespace ModelAnim {

namespace {

// Key k such that times[k] <= t < times[k + 1]; requires n >= 2 and
// times[0] <= t < times[n - 1].
quint32 findKey(const std::vector<float> &times, float t, quint32 &hint)
{
    const quint32 n = quint32(times.size());
    quint32 k = hint < n - 1 ? hint : 0;
    if (times[k] <= t && t < times[k + 1])
        return k;
    if (k + 2 < n && times[k + 1] <= t && t < times[k + 2])
        return hint = k + 1; // the next key: the usual case while playing
    const auto it = std::upper_bound(times.begin(), times.end(), t);
    k = quint32(std::max<std::ptrdiff_t>(0, (it - times.begin()) - 1));
    k = std::min(k, n - 2);
    hint = k;
    return k;
}

void normalize4(float *q)
{
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 0.0f) {
        for (int i = 0; i < 4; ++i)
            q[i] /= len;
    } else {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }
}

} // namespace

void sample(const ModelChannel &ch, float time, quint32 &hint, float *out)
{
    const bool rotation = ch.path == ModelChannelPath::Rotation;
    const int comps = rotation ? 4 : 3;
    const bool cubic = ch.interpolation == ModelInterpolation::CubicSpline;
    const size_t stride = size_t(comps) * (cubic ? 3 : 1);
    const size_t valueOffset = cubic ? size_t(comps) : 0; // [in, value, out] per key
    const quint32 n = quint32(ch.times.size());
    if (n == 0 || ch.values.size() < size_t(n) * stride) {
        for (int i = 0; i < comps; ++i)
            out[i] = (rotation && i == 3) ? 1.0f : 0.0f;
        return;
    }
    auto value = [&](quint32 k) { return ch.values.data() + size_t(k) * stride + valueOffset; };

    if (n == 1 || time <= ch.times.front() || time >= ch.times.back()) {
        const float *v = value(n == 1 || time <= ch.times.front() ? 0 : n - 1);
        std::copy_n(v, comps, out);
        if (rotation)
            normalize4(out);
        return;
    }

    const quint32 k = findKey(ch.times, time, hint);
    const float t0 = ch.times[k];
    const float t1 = ch.times[k + 1];
    const float dt = t1 - t0;
    const float s = dt > 0.0f ? (time - t0) / dt : 0.0f;
    const float *a = value(k);
    const float *b = value(k + 1);

    switch (ch.interpolation) {
    case ModelInterpolation::Step:
        std::copy_n(a, comps, out);
        break;
    case ModelInterpolation::Linear:
        if (rotation) {
            const QQuaternion qa(a[3], a[0], a[1], a[2]);
            const QQuaternion qb(b[3], b[0], b[1], b[2]);
            const QQuaternion q = QQuaternion::slerp(qa, qb, s); // shortest path
            out[0] = q.x();
            out[1] = q.y();
            out[2] = q.z();
            out[3] = q.scalar();
        } else {
            for (int i = 0; i < comps; ++i)
                out[i] = a[i] + (b[i] - a[i]) * s;
        }
        break;
    case ModelInterpolation::CubicSpline: {
        // glTF Hermite: out-tangent of k, in-tangent of k + 1, scaled by dt.
        const float *outTangent = ch.values.data() + size_t(k) * stride + 2 * size_t(comps);
        const float *inTangent = ch.values.data() + size_t(k + 1) * stride;
        const float s2 = s * s;
        const float s3 = s2 * s;
        const float h00 = 2 * s3 - 3 * s2 + 1;
        const float h10 = s3 - 2 * s2 + s;
        const float h01 = -2 * s3 + 3 * s2;
        const float h11 = s3 - s2;
        for (int i = 0; i < comps; ++i)
            out[i] = h00 * a[i] + h10 * dt * outTangent[i] + h01 * b[i] + h11 * dt * inTangent[i];
        break;
    }
    }
    if (rotation)
        normalize4(out);
}

} // namespace ModelAnim

void ModelPose::reset(const ModelDocument &doc)
{
    const size_t n = doc.nodes.size();
    m_translation.resize(n);
    m_rotation.resize(n);
    m_scale.resize(n);
    for (size_t i = 0; i < n; ++i) {
        m_translation[i] = doc.nodes[i].translation;
        m_rotation[i] = doc.nodes[i].rotation;
        m_scale[i] = doc.nodes[i].scale;
    }
    m_world.resize(n);
    m_hints.clear();
    m_hintsFor = nullptr;
}

void ModelPose::apply(const ModelAnimation &animation, float time)
{
    if (m_hintsFor != &animation) {
        m_hints.assign(animation.channels.size(), 0);
        m_hintsFor = &animation;
    }
    float v[4];
    for (size_t c = 0; c < animation.channels.size(); ++c) {
        const ModelChannel &ch = animation.channels[c];
        if (ch.node < 0 || size_t(ch.node) >= m_translation.size())
            continue;
        ModelAnim::sample(ch, time, m_hints[c], v);
        switch (ch.path) {
        case ModelChannelPath::Translation: m_translation[size_t(ch.node)] = QVector3D(v[0], v[1], v[2]); break;
        case ModelChannelPath::Rotation: m_rotation[size_t(ch.node)] = QQuaternion(v[3], v[0], v[1], v[2]); break;
        case ModelChannelPath::Scale: m_scale[size_t(ch.node)] = QVector3D(v[0], v[1], v[2]); break;
        }
    }
}

void ModelPose::updateWorld(const ModelDocument &doc, const QMatrix4x4 &root)
{
    const size_t n = std::min(doc.nodes.size(), m_translation.size());
    m_world.resize(doc.nodes.size());
    for (size_t i = 0; i < n; ++i) {
        const ModelNode &node = doc.nodes[i];
        QMatrix4x4 local;
        if (node.hasMatrix) {
            local = node.matrix;
        } else {
            local.translate(m_translation[i]);
            local.rotate(m_rotation[i]);
            local.scale(m_scale[i]);
        }
        m_world[i] = (node.parent >= 0 && size_t(node.parent) < i ? m_world[size_t(node.parent)] : root) * local;
    }
}

void ModelPose::jointMatrices(const ModelDocument &doc, const QMatrix4x4 &root, std::vector<float> &out,
                              std::vector<int> &skinBase) const
{
    size_t total = 0;
    skinBase.resize(doc.skins.size());
    for (size_t s = 0; s < doc.skins.size(); ++s) {
        skinBase[s] = int(total);
        total += doc.skins[s].joints.size();
    }
    out.resize(total * 16);
    float *dst = out.data();
    for (const ModelSkin &skin : doc.skins) {
        for (size_t j = 0; j < skin.joints.size(); ++j) {
            const int node = skin.joints[j];
            const QMatrix4x4 &world = node >= 0 && size_t(node) < m_world.size() ? m_world[size_t(node)] : root;
            const QMatrix4x4 m = j < skin.inverseBind.size() ? world * skin.inverseBind[j] : world;
            std::copy_n(m.constData(), 16, dst); // column-major
            dst += 16;
        }
    }
}
