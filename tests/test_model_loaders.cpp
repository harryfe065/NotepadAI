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

#include <QtTest>
#include <QBuffer>
#include <QDir>
#include <QImage>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtEndian>

#include "preview/model/ModelAnimator.h"
#include "preview/model/ModelCamera.h"
#include "preview/model/ModelDocument.h"
#include "preview/model/ModelLoader.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

void put32(QByteArray &b, quint32 v)
{
    char x[4];
    qToLittleEndian(v, x);
    b.append(x, 4);
}

void putF(QByteArray &b, float f)
{
    quint32 bits;
    std::memcpy(&bits, &f, 4);
    put32(b, bits);
}

bool fuzzy(float a, float b, float eps = 1e-4f)
{
    return std::abs(a - b) <= eps * std::max(1.0f, std::max(std::abs(a), std::abs(b)));
}

bool fuzzy(const QVector3D &a, const QVector3D &b, float eps = 1e-4f)
{
    return fuzzy(a.x(), b.x(), eps) && fuzzy(a.y(), b.y(), eps) && fuzzy(a.z(), b.z(), eps);
}

// Writes `bytes` to name in a temporary directory and loads it through the
// dispatch (format detection, caps, finalize).
ModelLoader::Result loadBytes(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
        return ModelLoader::failed(QStringLiteral("could not write %1").arg(path));
    f.close();
    std::atomic_bool cancel{false};
    return ModelLoader::load(path, cancel);
}

QByteArray binaryStl(const QByteArray &header80, int triangles)
{
    QByteArray b = header80.leftJustified(80, '\0', true);
    put32(b, quint32(triangles));
    for (int t = 0; t < triangles; ++t) {
        for (int i = 0; i < 3; ++i) putF(b, 0.0f);   // stored normal (ignored)
        const float o = float(t);
        const float v[9] = {o, 0, 0, o + 1, 0, 0, o, 1, 2};
        for (float x : v) putF(b, x);
        b.append(2, '\0');                            // attribute
    }
    return b;
}

// MagicaVoxel: MAIN with one SIZE + XYZI pair.
QByteArray voxFile(int sx, int sy, int sz, const QVector<QVector<int>> &voxels)
{
    QByteArray children;
    children.append("SIZE", 4);
    put32(children, 12);
    put32(children, 0);
    put32(children, quint32(sx));
    put32(children, quint32(sy));
    put32(children, quint32(sz));
    children.append("XYZI", 4);
    put32(children, quint32(4 + voxels.size() * 4));
    put32(children, 0);
    put32(children, quint32(voxels.size()));
    for (const QVector<int> &v : voxels)
        for (int c : v) children.append(char(c));

    QByteArray b("VOX ");
    put32(b, 150);
    b.append("MAIN", 4);
    put32(b, 0);
    put32(b, quint32(children.size()));
    b += children;
    return b;
}

// One triangle, positions only, as a glTF buffer.
QByteArray trianglePositions()
{
    QByteArray b;
    const float v[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (float x : v) putF(b, x);
    return b;
}

QByteArray gltfJson(const QByteArray &bufferEntry)
{
    return QByteArray(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"mesh":0,"translation":[2,0,0]}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
"buffers":[)") + bufferEntry + QByteArray(R"(],
"bufferViews":[{"buffer":0,"byteLength":36}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}]})");
}

// One primitive over one data-URI buffer holding trianglePositions(); the
// caller supplies the primitive, buffer views and accessors.
QByteArray gltfDoc(const QByteArray &primitive, const QByteArray &bufferViews, const QByteArray &accessors)
{
    return QByteArray(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)")
           + R"("meshes":[{"primitives":[)" + primitive + R"(]}],)"
           + R"("buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,)"
           + trianglePositions().toBase64() + R"("}],"bufferViews":)" + bufferViews + R"(,"accessors":)" + accessors
           + "}";
}

const QByteArray kPositionView = R"([{"buffer":0,"byteLength":36}])";
const QByteArray kPositionAccessor = R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"})";

QByteArray voxChunk(const char *id, const QByteArray &content)
{
    QByteArray b(id, 4);
    put32(b, quint32(content.size()));
    put32(b, 0);
    return b + content;
}

// MagicaVoxel nodes; dictionaries are empty.
QByteArray voxTransform(int id, int child)
{
    QByteArray c;
    put32(c, quint32(id));
    put32(c, 0);              // attributes
    put32(c, quint32(child));
    put32(c, 0xFFFFFFFFu);    // reserved
    put32(c, 0xFFFFFFFFu);    // layer
    put32(c, 1);              // frames
    put32(c, 0);              // frame attributes
    return voxChunk("nTRN", c);
}

QByteArray voxGroup(int id, const QVector<int> &children)
{
    QByteArray c;
    put32(c, quint32(id));
    put32(c, 0);
    put32(c, quint32(children.size()));
    for (int child : children)
        put32(c, quint32(child));
    return voxChunk("nGRP", c);
}

QByteArray voxShape(int id, int model)
{
    QByteArray c;
    put32(c, quint32(id));
    put32(c, 0);
    put32(c, 1);              // models
    put32(c, quint32(model));
    put32(c, 0);              // model attributes
    return voxChunk("nSHP", c);
}

ModelChannel channel(ModelChannelPath path, ModelInterpolation interp, std::vector<float> times,
                     std::vector<float> values)
{
    ModelChannel c;
    c.node = 0;
    c.path = path;
    c.interpolation = interp;
    c.times = std::move(times);
    c.values = std::move(values);
    return c;
}

} // namespace

class TestModelLoaders : public QObject
{
    Q_OBJECT

private slots:
    // --- packing and scene helpers ---
    void packedNormalRoundTrips();
    void skinWeightsKeepTopFourAndSumTo255();
    void zUpRootRotatesZIntoY();

    // --- camera ---
    void cameraFramesTheBoundingSphere();
    void cameraViewPresets();
    void cameraPanMovesTargetSideways();
    void cameraZoomClamps();
    void orthographicMatchesPerspectiveAtTarget();

    // --- animation ---
    void sampleStepAndLinear();
    void sampleClampsOutsideTheKeys();
    void sampleCubicSpline();
    void sampleRotationSlerps();
    void sampleHintSurvivesSeeks();
    void poseJointMatrices();

    // --- format detection ---
    void sniffers();

    // --- loaders ---
    void stlBinary();
    void stlBinaryWithSolidHeader();
    void stlAscii();
    void plyAsciiQuadWithColours();
    void plyPointCloud();
    void voxSingleVoxel();
    void voxGreedyMergesARow();
    void gltfDataUri();
    void glbBinaryChunk();
    void gltfMapsOnAnotherUvSet();
    void gltfGodotCollidersAreHidden();
    void objWeldsAndTriangulates();
    void objRejectsCompiledObjectFiles();
    void emptyAndUnknownFiles();

    // --- hostile inputs ---
    void gltfStripFanAndLoopExpand();
    void gltfHugeAccessorCountIsRejected();
    void gltfHugeStripWithoutBufferViewIsTooLarge();
    void gltfBufferOutsideTheFolderIsRefused();
    void plyHugeElementAndListCounts();
    void voxSelfReferencingGroupVisitsOnce();
    void objManyUvsOnOnePositionWeldsFast();

private:
    QTemporaryDir m_dir;
};

void TestModelLoaders::packedNormalRoundTrips()
{
    const quint32 p = ModelPack::normal(0.0f, 0.0f, 2.0f); // any length: normalised
    auto field = [p](int shift) {
        int v = int((p >> shift) & 0x3FF);
        return v >= 512 ? v - 1024 : v;
    };
    QCOMPARE(field(0), 0);
    QCOMPARE(field(10), 0);
    QCOMPARE(field(20), 511);
    // Degenerate input falls back to +Y instead of NaN.
    const quint32 up = ModelPack::normal(0.0f, 0.0f, 0.0f);
    QCOMPARE(int((up >> 10) & 0x3FF), 511);
}

void TestModelLoaders::skinWeightsKeepTopFourAndSumTo255()
{
    const quint32 joints[5] = {7, 3, 9, 1, 4};
    const float weights[5] = {0.05f, 0.4f, 0.3f, 0.2f, 0.1f};
    const ModelSkinVertex v = ModelPack::skinVertex(joints, weights, 5);
    int sum = 0;
    bool hasLightest = false;
    for (int i = 0; i < 4; ++i) {
        sum += v.weights[i];
        if (v.joints[i] == 7 && v.weights[i] > 0)
            hasLightest = true;
    }
    QCOMPARE(sum, 255);
    QVERIFY(!hasLightest);

    const float zero[2] = {0.0f, 0.0f};
    const ModelSkinVertex none = ModelPack::skinVertex(joints, zero, 2);
    QCOMPARE(int(none.weights[0]) + none.weights[1] + none.weights[2] + none.weights[3], 255);
}

void TestModelLoaders::zUpRootRotatesZIntoY()
{
    const QMatrix4x4 root = ModelScene::rootTransform(true);
    QVERIFY(fuzzy(root.map(QVector3D(0, 0, 1)), QVector3D(0, 1, 0)));
    QVERIFY(fuzzy(root.map(QVector3D(0, 1, 0)), QVector3D(0, 0, -1)));
    QVERIFY(ModelScene::rootTransform(false).isIdentity());
}

void TestModelLoaders::cameraFramesTheBoundingSphere()
{
    ModelCamera cam;
    cam.setBounds(QVector3D(-1, -1, -1), QVector3D(3, 1, 1));
    cam.frame(1.0f);
    QVERIFY(fuzzy(cam.target(), QVector3D(1, 0, 0)));
    const float r = std::sqrt(4.0f + 1.0f + 1.0f);
    QVERIFY(fuzzy(cam.radius(), r));
    const float half = qDegreesToRadians(ModelCamera::kFovY) * 0.5f;
    QVERIFY(fuzzy(cam.distance(), r * 1.08f / std::sin(half)));

    // A tall narrow viewport is limited by the horizontal fov: farther away.
    const float square = cam.distance();
    cam.frame(0.5f);
    QVERIFY(cam.distance() > square);
    // The sphere still fits: its angular radius is within the horizontal half-fov.
    const float halfH = std::atan(std::tan(half) * 0.5f);
    QVERIFY(std::asin(r / cam.distance()) <= halfH + 1e-5f);
}

void TestModelLoaders::cameraViewPresets()
{
    ModelCamera cam;
    cam.setBounds(QVector3D(-1, -1, -1), QVector3D(1, 1, 1));
    cam.setView(ModelCamera::View::Front, 1.0f);
    QVERIFY(fuzzy(cam.forward(), QVector3D(0, 0, -1)));
    QVERIFY(fuzzy(cam.eye(), QVector3D(0, 0, cam.distance())));
    cam.setView(ModelCamera::View::Right, 1.0f);
    QVERIFY(fuzzy(cam.forward(), QVector3D(-1, 0, 0)));
    cam.setView(ModelCamera::View::Back, 1.0f);
    QVERIFY(fuzzy(cam.forward(), QVector3D(0, 0, 1)));
    cam.setView(ModelCamera::View::Top, 1.0f);
    QVERIFY(cam.forward().y() < -0.999f);
    // The view matrix stays well defined just short of the pole.
    const QMatrix4x4 view = cam.viewMatrix();
    QVERIFY(fuzzy(view.map(cam.target()), QVector3D(0, 0, -cam.distance()), 1e-3f));
}

void TestModelLoaders::cameraPanMovesTargetSideways()
{
    ModelCamera cam;
    cam.setBounds(QVector3D(-1, -1, -1), QVector3D(1, 1, 1));
    cam.setView(ModelCamera::View::Front, 1.0f);
    const float upp = cam.unitsPerPixel(500);
    cam.pan(10.0f, 0.0f, 500); // drag right: the scene follows, the target moves left
    QVERIFY(fuzzy(cam.target(), QVector3D(-10.0f * upp, 0, 0)));
    cam.pan(0.0f, 10.0f, 500); // drag down
    QVERIFY(fuzzy(cam.target(), QVector3D(-10.0f * upp, 10.0f * upp, 0)));
}

void TestModelLoaders::cameraZoomClamps()
{
    ModelCamera cam;
    cam.setBounds(QVector3D(-1, -1, -1), QVector3D(1, 1, 1));
    cam.frame(1.0f);
    const float d = cam.distance();
    cam.zoom(1.0f);
    QVERIFY(fuzzy(cam.distance(), d * 0.85f));
    cam.zoom(1000.0f);
    QVERIFY(fuzzy(cam.distance(), cam.radius() * 1e-3f));
    cam.zoom(-1000.0f);
    QVERIFY(fuzzy(cam.distance(), cam.radius() * 1e3f));
    QVERIFY(cam.nearPlane() > 0.0f && cam.nearPlane() < cam.farPlane());
}

void TestModelLoaders::orthographicMatchesPerspectiveAtTarget()
{
    ModelCamera cam;
    cam.setBounds(QVector3D(-1, -1, -1), QVector3D(1, 1, 1));
    cam.setView(ModelCamera::View::Front, 1.0f);
    // A point at the target's depth projects to the same place in both modes.
    const QVector3D p(0.5f, 0.25f, 0.0f);
    const QVector3D persp = (cam.projectionMatrix(1.5f) * cam.viewMatrix()).map(p);
    cam.setOrthographic(true);
    const QVector3D ortho = (cam.projectionMatrix(1.5f) * cam.viewMatrix()).map(p);
    QVERIFY(fuzzy(persp.x(), ortho.x(), 1e-3f));
    QVERIFY(fuzzy(persp.y(), ortho.y(), 1e-3f));
    // Nothing between the camera and the target is clipped in ortho.
    QVERIFY(cam.nearPlane() < 0.0f);
}

void TestModelLoaders::sampleStepAndLinear()
{
    float out[4];
    quint32 hint = 0;
    const ModelChannel lin = channel(ModelChannelPath::Translation, ModelInterpolation::Linear, {0.0f, 1.0f},
                                     {0, 0, 0, 2, 4, 6});
    ModelAnim::sample(lin, 0.5f, hint, out);
    QVERIFY(fuzzy(QVector3D(out[0], out[1], out[2]), QVector3D(1, 2, 3)));

    const ModelChannel step = channel(ModelChannelPath::Scale, ModelInterpolation::Step, {0.0f, 1.0f, 2.0f},
                                      {1, 1, 1, 2, 2, 2, 3, 3, 3});
    ModelAnim::sample(step, 0.99f, hint, out);
    QCOMPARE(out[0], 1.0f);
    ModelAnim::sample(step, 1.0f, hint, out);
    QCOMPARE(out[0], 2.0f);
    ModelAnim::sample(step, 1.5f, hint, out);
    QCOMPARE(out[0], 2.0f);
}

void TestModelLoaders::sampleClampsOutsideTheKeys()
{
    float out[4];
    quint32 hint = 0;
    const ModelChannel c = channel(ModelChannelPath::Translation, ModelInterpolation::Linear, {1.0f, 2.0f},
                                   {5, 0, 0, 7, 0, 0});
    ModelAnim::sample(c, 0.0f, hint, out);
    QCOMPARE(out[0], 5.0f);
    ModelAnim::sample(c, 9.0f, hint, out);
    QCOMPARE(out[0], 7.0f);

    // Short value arrays never read out of bounds: the identity comes back.
    const ModelChannel broken = channel(ModelChannelPath::Rotation, ModelInterpolation::Linear, {0.0f, 1.0f}, {0, 0});
    ModelAnim::sample(broken, 0.5f, hint, out);
    QCOMPARE(out[3], 1.0f);
}

void TestModelLoaders::sampleCubicSpline()
{
    float out[4];
    quint32 hint = 0;
    // [in, value, out] per key; zero tangents give the smoothstep 0 -> 1.
    const ModelChannel flat = channel(ModelChannelPath::Translation, ModelInterpolation::CubicSpline, {0.0f, 2.0f},
                                      {0, 0, 0, 0, 0, 0, 0, 0, 0, /* key 1 */ 0, 0, 0, 1, 1, 1, 0, 0, 0});
    ModelAnim::sample(flat, 1.0f, hint, out);
    QVERIFY(fuzzy(out[0], 0.5f));
    ModelAnim::sample(flat, 0.5f, hint, out);
    QVERIFY(fuzzy(out[0], 0.15625f)); // 3s^2 - 2s^3 at s = 0.25

    // Out-tangent 1 per second over a 2 s span, value 0 -> 0: h10 * dt * m.
    const ModelChannel slope = channel(ModelChannelPath::Translation, ModelInterpolation::CubicSpline, {0.0f, 2.0f},
                                       {0, 0, 0, 0, 0, 0, 1, 0, 0, /* key 1 */ 0, 0, 0, 0, 0, 0, 0, 0, 0});
    ModelAnim::sample(slope, 1.0f, hint, out);
    QVERIFY(fuzzy(out[0], 0.125f * 2.0f));
}

void TestModelLoaders::sampleRotationSlerps()
{
    float out[4];
    quint32 hint = 0;
    const QQuaternion q90 = QQuaternion::fromAxisAndAngle(0, 1, 0, 90);
    const ModelChannel c = channel(ModelChannelPath::Rotation, ModelInterpolation::Linear, {0.0f, 1.0f},
                                   {0, 0, 0, 1, q90.x(), q90.y(), q90.z(), q90.scalar()});
    ModelAnim::sample(c, 0.5f, hint, out);
    const QQuaternion q(out[3], out[0], out[1], out[2]);
    const QQuaternion expect = QQuaternion::fromAxisAndAngle(0, 1, 0, 45);
    QVERIFY(std::abs(QQuaternion::dotProduct(q, expect)) > 0.99999f);
    QVERIFY(fuzzy(q.length(), 1.0f));
}

void TestModelLoaders::sampleHintSurvivesSeeks()
{
    std::vector<float> times;
    std::vector<float> values;
    for (int i = 0; i <= 100; ++i) {
        times.push_back(float(i) * 0.1f);
        values.insert(values.end(), {float(i), 0, 0});
    }
    const ModelChannel c = channel(ModelChannelPath::Translation, ModelInterpolation::Linear, times, values);
    quint32 hint = 0;
    float out[4];
    const float seq[] = {0.05f, 0.15f, 0.25f, 7.25f, 0.35f, 9.95f, 0.0f, 5.05f};
    for (const float t : seq) {
        ModelAnim::sample(c, t, hint, out);
        QVERIFY2(fuzzy(out[0], t * 10.0f, 1e-3f), qPrintable(QStringLiteral("t=%1 got %2").arg(t).arg(out[0])));
    }
    hint = 12345; // any hint value is valid
    ModelAnim::sample(c, 3.05f, hint, out);
    QVERIFY(fuzzy(out[0], 30.5f, 1e-3f));
}

void TestModelLoaders::poseJointMatrices()
{
    ModelDocument doc;
    ModelNode root;
    root.translation = QVector3D(0, 1, 0);
    ModelNode child;
    child.parent = 0;
    child.translation = QVector3D(2, 0, 0);
    doc.nodes = {root, child};
    ModelSkin skin;
    skin.joints = {1};
    QMatrix4x4 inverseBind;
    inverseBind.translate(-2, -1, 0);
    skin.inverseBind = {inverseBind};
    doc.skins = {skin};
    ModelAnimation anim;
    anim.duration = 1.0f;
    ModelChannel move = channel(ModelChannelPath::Translation, ModelInterpolation::Linear, {0.0f, 1.0f},
                                {2, 0, 0, 4, 0, 0});
    move.node = 1;
    anim.channels.push_back(move);

    ModelPose pose;
    pose.reset(doc);
    pose.updateWorld(doc, QMatrix4x4());
    QVERIFY(fuzzy(pose.world()[1].map(QVector3D()), QVector3D(2, 1, 0)));
    std::vector<float> joints;
    std::vector<int> base;
    pose.jointMatrices(doc, QMatrix4x4(), joints, base);
    QCOMPARE(joints.size(), size_t(16));
    QVERIFY(base == std::vector<int>{0});
    // In the bind pose, joint x inverse bind is the identity.
    QVERIFY(QMatrix4x4(joints.data()).transposed().isIdentity());

    pose.apply(anim, 1.0f);
    pose.updateWorld(doc, QMatrix4x4());
    pose.jointMatrices(doc, QMatrix4x4(), joints, base);
    const QMatrix4x4 m = QMatrix4x4(joints.data()).transposed(); // the buffer is column-major
    QVERIFY(fuzzy(m.map(QVector3D(2, 1, 0)), QVector3D(4, 1, 0)));
}

void TestModelLoaders::sniffers()
{
    QByteArray glb("glTF");
    put32(glb, 2);
    put32(glb, 12);
    QVERIFY(ModelLoader::looksLikeGlb(glb));
    QByteArray glb1("glTF");
    put32(glb1, 1);
    put32(glb1, 12);
    QVERIFY(!ModelLoader::looksLikeGlb(glb1)); // glTF 1.0 is not supported

    QVERIFY(ModelLoader::looksLikeFbx(QByteArray("Kaydara FBX Binary  \0\x1a\0", 23)));
    QVERIFY(ModelLoader::looksLikeFbx("; FBX 7.4.0 project file\n"));
    QVERIFY(ModelLoader::looksLikePly("ply\nformat ascii 1.0\n"));
    QVERIFY(!ModelLoader::looksLikePly("plywood"));
    QVERIFY(ModelLoader::looksLikeVox(voxFile(1, 1, 1, {{0, 0, 0, 1}})));
    QVERIFY(!ModelLoader::looksLikeVox("VOX "));

    QCOMPARE(ModelLoader::formatFor(QStringLiteral("glb"), {}), ModelLoader::Format::Gltf);
    QCOMPARE(ModelLoader::formatFor(QStringLiteral("STL").toLower(), {}), ModelLoader::Format::Stl);
    QCOMPARE(ModelLoader::formatFor(QString(), glb), ModelLoader::Format::Gltf);
    QCOMPARE(ModelLoader::formatFor(QStringLiteral("bin"), "nothing"), ModelLoader::Format::Unknown);
}

void TestModelLoaders::stlBinary()
{
    QVERIFY(m_dir.isValid());
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("two.stl"), binaryStl("exported", 2));
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->stats.triangles, quint64(2));
    QCOMPARE(r.doc->vertices.size(), size_t(6));
    QVERIFY(r.doc->zUp);
    QCOMPARE(r.doc->primitives.size(), size_t(1));
    QVERIFY(r.doc->primitives[0].flatNormals);
    QCOMPARE(r.doc->primitives[0].indexCount, 0u);
    QVERIFY(!r.doc->primitives[0].hasColor);
    QVERIFY(fuzzy(r.doc->primitives[0].boundsMax, QVector3D(2, 1, 2)));
}

void TestModelLoaders::stlBinaryWithSolidHeader()
{
    // Many exporters start binary headers with "solid"; the size decides.
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("solid.stl"), binaryStl("solid model", 3));
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->stats.triangles, quint64(3));
}

void TestModelLoaders::stlAscii()
{
    const QByteArray text = "solid t\n"
                            " facet normal 0 0 1\n  outer loop\n"
                            "   vertex 0 0 0\n   vertex 1.5e0 0 0\n   vertex 0 -2.25 0\n"
                            "  endloop\n endfacet\n"
                            "endsolid t\n";
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("ascii.stl"), text);
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->stats.triangles, quint64(1));
    QCOMPARE(r.doc->vertices[1].px, 1.5f);
    QCOMPARE(r.doc->vertices[2].py, -2.25f);
}

void TestModelLoaders::plyAsciiQuadWithColours()
{
    const QByteArray text = "ply\nformat ascii 1.0\ncomment test\n"
                            "element vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
                            "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                            "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
                            "0 0 0 255 0 0\n1 0 0 0 255 0\n1 1 0 0 0 255\n0 1 0 255 255 255\n"
                            "4 0 1 2 3\n";
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("quad.ply"), text);
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->stats.triangles, quint64(2));
    QCOMPARE(r.doc->stats.vertices, quint64(4));
    QVERIFY(r.doc->primitives[0].hasColor);
    QCOMPARE(r.doc->vertices[0].color, ModelPack::colorFromBytes(255, 0, 0, 255));
}

void TestModelLoaders::plyPointCloud()
{
    QByteArray b = "ply\nformat binary_little_endian 1.0\nelement vertex 3\n"
                   "property float x\nproperty float y\nproperty float z\nend_header\n";
    for (int i = 0; i < 9; ++i)
        putF(b, float(i));
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("cloud.ply"), b);
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->primitives.size(), size_t(1));
    QCOMPARE(r.doc->primitives[0].topology, ModelTopology::Points);
    QCOMPARE(r.doc->stats.points, quint64(3));
    QCOMPARE(r.doc->vertices[2].pz, 8.0f);
}

void TestModelLoaders::voxSingleVoxel()
{
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("one.vox"), voxFile(3, 3, 3, {{1, 1, 1, 5}}));
    QVERIFY2(r.doc, qPrintable(r.error));
    QVERIFY(r.doc->zUp);
    QCOMPARE(r.doc->stats.triangles, quint64(12)); // six faces, two triangles each
    QCOMPARE(r.doc->stats.vertices, quint64(24));
    // Pivot at floor(size / 2) = 1: the voxel spans [0, 1] on every axis.
    const ModelPrimitive &p = r.doc->primitives[0];
    QVERIFY(fuzzy(p.boundsMin, QVector3D(0, 0, 0)));
    QVERIFY(fuzzy(p.boundsMax, QVector3D(1, 1, 1)));
}

void TestModelLoaders::voxGreedyMergesARow()
{
    // Four equal voxels in a row are still one box: six quads.
    const ModelLoader::Result r = loadBytes(
        m_dir, QStringLiteral("row.vox"), voxFile(4, 1, 1, {{0, 0, 0, 9}, {1, 0, 0, 9}, {2, 0, 0, 9}, {3, 0, 0, 9}}));
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->stats.triangles, quint64(12));
    // Two colours split the faces they colour: the long sides and the ends.
    const ModelLoader::Result two = loadBytes(
        m_dir, QStringLiteral("two.vox"), voxFile(2, 1, 1, {{0, 0, 0, 9}, {1, 0, 0, 10}}));
    QVERIFY2(two.doc, qPrintable(two.error));
    QCOMPARE(two.doc->stats.triangles, quint64(20)); // 2 ends + 4 sides x 2 colours = 10 quads
}

void TestModelLoaders::gltfDataUri()
{
    const QByteArray entry = QByteArray(R"({"byteLength":36,"uri":"data:application/octet-stream;base64,)")
                             + trianglePositions().toBase64() + "\"}";
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("tri.gltf"), gltfJson(entry));
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->stats.triangles, quint64(1));
    QVERIFY(r.doc->cullBackFaces);
    QVERIFY(!r.doc->zUp);
    QCOMPARE(r.doc->nodes.size(), size_t(1));
    QVERIFY(fuzzy(r.doc->nodes[0].translation, QVector3D(2, 0, 0)));
    QVERIFY(r.doc->primitives[0].flatNormals); // no NORMAL attribute

    // The scene bounds include the node transform.
    const std::vector<QMatrix4x4> world = ModelScene::restWorld(*r.doc, QMatrix4x4());
    QVector3D min, max;
    QVERIFY(ModelScene::sceneBounds(*r.doc, world, QMatrix4x4(), min, max));
    QVERIFY(fuzzy(min, QVector3D(2, 0, 0)));
    QVERIFY(fuzzy(max, QVector3D(3, 1, 0)));
}

void TestModelLoaders::glbBinaryChunk()
{
    QByteArray json = gltfJson(R"({"byteLength":36})");
    while (json.size() % 4)
        json.append(' ');
    const QByteArray bin = trianglePositions();
    QByteArray glb("glTF");
    put32(glb, 2);
    put32(glb, quint32(12 + 8 + json.size() + 8 + bin.size()));
    put32(glb, quint32(json.size()));
    put32(glb, 0x4E4F534A); // JSON
    glb += json;
    put32(glb, quint32(bin.size()));
    put32(glb, 0x004E4942); // BIN
    glb += bin;
    // No extension: the sniffed magic picks the loader.
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("tri_noext"), glb);
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->stats.triangles, quint64(1));
}

void TestModelLoaders::gltfMapsOnAnotherUvSet()
{
    // Baked atlases often sit on a later UV set (Blender: TEXCOORD_2) while
    // TEXCOORD_0 holds the modelling UVs; the material names the set.
    QByteArray bin = trianglePositions();
    for (int i = 0; i < 6; ++i) putF(bin, 0.0f);        // TEXCOORD_0
    const float uv1[6] = {0.25f, 0.5f, 0.75f, 0.5f, 0.25f, 1.0f};
    for (float x : uv1) putF(bin, x);                   // TEXCOORD_1
    QImage pixel(1, 1, QImage::Format_RGB32);
    pixel.fill(Qt::red);
    QByteArray png;
    QBuffer pngOut(&png);
    pngOut.open(QIODevice::WriteOnly);
    QVERIFY(pixel.save(&pngOut, "PNG"));

    const QByteArray uv = R"(,{"bufferView":%1,"componentType":5126,"count":3,"type":"VEC2"})";
    const QByteArray json = QByteArray(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)")
        + R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"TEXCOORD_1":2},"material":0}]}],)"
        + R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0,"texCoord":1}},)"
        + R"("normalTexture":{"index":0,"texCoord":1}}],)"
        + R"("textures":[{"source":0}],"images":[{"uri":"data:image/png;base64,)" + png.toBase64() + R"("}],)"
        + R"("buffers":[{"byteLength":84,"uri":"data:application/octet-stream;base64,)" + bin.toBase64() + R"("}],)"
        + R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24},)"
        + R"({"buffer":0,"byteOffset":60,"byteLength":24}],"accessors":[)" + kPositionAccessor
        + QByteArray(uv).replace("%1", "1") + QByteArray(uv).replace("%1", "2") + "]}";
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("uv1.gltf"), json);
    QVERIFY2(r.doc, qPrintable(r.error));
    QVERIFY2(r.doc->warnings.isEmpty(), qPrintable(r.doc->warnings.join(u'; ')));
    QVERIFY(r.doc->materials[0].baseColorTexture >= 0);
    QVERIFY(r.doc->materials[0].normalTexture >= 0);
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(r.doc->vertices[size_t(i)].u, uv1[i * 2]);
        QCOMPARE(r.doc->vertices[size_t(i)].v, uv1[i * 2 + 1]);
    }
}

void TestModelLoaders::gltfGodotCollidersAreHidden()
{
    // Godot's "-colonly" / "-convcolonly" name suffixes mark collision-only
    // nodes: Godot never draws them, so neither does the preview.
    const QByteArray json = QByteArray(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1,2,3]}],)")
        + R"("nodes":[{"mesh":0,"name":"Temple"},{"mesh":0,"name":"Wall-convcolonly"},)"
        + R"({"mesh":0,"name":"Ground_colonly"},{"mesh":0,"name":"Rail-col"}],)"
        + R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
        + R"("buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,)" + trianglePositions().toBase64()
        + R"("}],"bufferViews":)" + kPositionView + R"(,"accessors":[)" + kPositionAccessor + "]}";
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("godot.gltf"), json);
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->nodes.size(), size_t(4));
    QCOMPARE(r.doc->nodes[0].mesh, 0);
    QCOMPARE(r.doc->nodes[1].mesh, -1);
    QCOMPARE(r.doc->nodes[2].mesh, -1);
    QCOMPARE(r.doc->nodes[3].mesh, 0); // "-col" keeps its visible mesh in Godot
}

void TestModelLoaders::objWeldsAndTriangulates()
{
    const QByteArray text = "# quad\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                            "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                            "f 1/1 2/2 3/3 4/4\n"
                            "f 1/1 3/3 2/2\n";
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("quad.obj"), text);
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->stats.triangles, quint64(3));
    QCOMPARE(r.doc->stats.vertices, quint64(4)); // shared corners are welded
    QVERIFY(r.doc->primitives[0].flatNormals);
    // V is flipped to the top-left origin.
    for (const ModelVertex &v : r.doc->vertices) {
        if (v.px == 0.0f && v.py == 1.0f)
            QCOMPARE(v.v, 0.0f);
    }
}

void TestModelLoaders::objRejectsCompiledObjectFiles()
{
    QByteArray coff("\x64\x86\x03\x00", 4);
    coff.append(60, '\0');
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("main.obj"), coff);
    QVERIFY(!r.doc);
    QVERIFY(!r.error.isEmpty());
}

void TestModelLoaders::emptyAndUnknownFiles()
{
    QVERIFY(!loadBytes(m_dir, QStringLiteral("empty.glb"), QByteArray()).doc);
    QVERIFY(!loadBytes(m_dir, QStringLiteral("junk.stl"), QByteArray("not an stl")).doc);
    QVERIFY(!loadBytes(m_dir, QStringLiteral("broken.gltf"), QByteArray("{\"asset\":")).doc);
    QVERIFY(!loadBytes(m_dir, QStringLiteral("short.vox"), QByteArray("VOX \x96\0\0\0MAIN", 12)).doc);
}

void TestModelLoaders::gltfStripFanAndLoopExpand()
{
    const QByteArray accessors = "[" + kPositionAccessor + "]";
    const ModelLoader::Result strip = loadBytes(
        m_dir, QStringLiteral("strip.gltf"), gltfDoc(R"({"attributes":{"POSITION":0},"mode":5})", kPositionView, accessors));
    QVERIFY2(strip.doc, qPrintable(strip.error));
    QCOMPARE(strip.doc->stats.triangles, quint64(1));
    const ModelLoader::Result fan = loadBytes(
        m_dir, QStringLiteral("fan.gltf"), gltfDoc(R"({"attributes":{"POSITION":0},"mode":6})", kPositionView, accessors));
    QVERIFY2(fan.doc, qPrintable(fan.error));
    QCOMPARE(fan.doc->stats.triangles, quint64(1));
    const ModelLoader::Result loop = loadBytes(
        m_dir, QStringLiteral("loop.gltf"), gltfDoc(R"({"attributes":{"POSITION":0},"mode":2})", kPositionView, accessors));
    QVERIFY2(loop.doc, qPrintable(loop.error));
    QCOMPARE(loop.doc->stats.lines, quint64(3)); // closed: 0-1, 1-2, 2-0
    QCOMPARE(loop.doc->indices.size(), size_t(6));
    QCOMPARE(loop.doc->indices[5], 0u);
}

void TestModelLoaders::gltfHugeAccessorCountIsRejected()
{
    // 16 * (2^60 + 1 - 1) + 12 wraps to 12 bytes in size_t: cgltf_validate
    // alone accepts it against a 36-byte view.
    const ModelLoader::Result wrapped = loadBytes(
        m_dir, QStringLiteral("wrap.gltf"),
        gltfDoc(R"({"attributes":{"POSITION":0}})", R"([{"buffer":0,"byteLength":36,"byteStride":16}])",
                R"([{"bufferView":0,"componentType":5126,"count":1152921504606846977,"type":"VEC3"}])"));
    QVERIFY(!wrapped.doc);
    QVERIFY(!wrapped.tooLarge);
    QVERIFY(!wrapped.error.isEmpty());

    // Zero-filled (no buffer view) matrices: only the size cap bounds them.
    const ModelLoader::Result zeros = loadBytes(
        m_dir, QStringLiteral("zeros.gltf"),
        gltfDoc(R"({"attributes":{"POSITION":0}})", kPositionView,
                "[" + kPositionAccessor + R"(,{"componentType":5126,"count":4611686018427387904,"type":"MAT4"}])"));
    QVERIFY(!zeros.doc);
    QVERIFY(zeros.tooLarge);
}

void TestModelLoaders::gltfHugeStripWithoutBufferViewIsTooLarge()
{
    // (m - 2) * 3 wraps to 2 in 32 bits for this count.
    for (const QByteArray &count : {QByteArray("1431655768"), QByteArray("200000000")}) {
        QElapsedTimer timer;
        timer.start();
        const ModelLoader::Result r = loadBytes(
            m_dir, QStringLiteral("hugestrip.gltf"),
            gltfDoc(R"({"attributes":{"POSITION":0},"indices":1,"mode":5})", kPositionView,
                    "[" + kPositionAccessor + R"(,{"componentType":5125,"count":)" + count + R"(,"type":"SCALAR"}])"));
        QVERIFY(!r.doc);
        QVERIFY2(r.tooLarge, qPrintable(r.error));
        QVERIFY(timer.elapsed() < 5000); // refused before anything is allocated
    }
}

void TestModelLoaders::gltfBufferOutsideTheFolderIsRefused()
{
    for (const char *uri : {"../x.bin", "sub/../../x.bin", "%2E%2E/x.bin", "/etc/x.bin", "//host/share/x.bin",
                            "C:/x.bin", "http://example.com/x.bin", "x.bin:stream", "x.bin."}) {
        const QByteArray json = QByteArray(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36,"uri":")") + uri
                                + R"("}]})";
        const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("outside.gltf"), json);
        QVERIFY2(!r.doc, uri);
        QVERIFY2(r.error.contains(QLatin1String("folder")), qPrintable(r.error));
    }
    // Inside the folder (after "..") is still fine.
    QVERIFY(m_dir.isValid());
    QDir(m_dir.path()).mkpath(QStringLiteral("bufs"));
    QFile bin(m_dir.filePath(QStringLiteral("bufs/tri.bin")));
    QVERIFY(bin.open(QIODevice::WriteOnly));
    bin.write(trianglePositions());
    bin.close();
    const ModelLoader::Result ok =
        loadBytes(m_dir, QStringLiteral("inside.gltf"), gltfJson(R"({"byteLength":36,"uri":"bufs/../bufs/tri.bin"})"));
    QVERIFY2(ok.doc, qPrintable(ok.error));
    QCOMPARE(ok.doc->stats.triangles, quint64(1));
}

void TestModelLoaders::plyHugeElementAndListCounts()
{
    const QByteArray header = "ply\nformat binary_little_endian 1.0\nelement vertex 3\n"
                              "property float x\nproperty float y\nproperty float z\n";
    QByteArray vertices;
    for (int i = 0; i < 9; ++i)
        putF(vertices, float(i));

    // 4 * (2^61 + 1) is negative as qint64: the walk must not move backwards.
    {
        QByteArray b = header + "element junk 2305843009213693953\nproperty int v\n"
                                "element face 1\nproperty list uchar int vertex_indices\nend_header\n";
        b += vertices;
        put32(b, 0);
        put32(b, 0);
        const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("junk.ply"), b);
        QVERIFY2(r.doc, qPrintable(r.error));
        QCOMPARE(r.doc->stats.points, quint64(3));
    }
    // A float list count of NaN (and later 1e30) is never converted.
    for (const float count : {std::numeric_limits<float>::quiet_NaN(), 1e30f, -5.0f}) {
        QByteArray b = header + "element face 1\nproperty list float int vertex_indices\nend_header\n";
        b += vertices;
        putF(b, count);
        for (int i = 0; i < 3; ++i)
            put32(b, quint32(i));
        const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("floatcount.ply"), b);
        QVERIFY2(r.doc, qPrintable(r.error));
        QCOMPARE(r.doc->stats.triangles, quint64(0));
        QCOMPARE(r.doc->stats.points, quint64(3));
    }
    // ASCII: an infinite list count, and a face count the file cannot hold.
    {
        const QByteArray text = "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\n"
                                "property float z\nelement face 1\nproperty list uchar int vertex_indices\nend_header\n"
                                "0 0 0\n1 0 0\n0 1 0\n1e300 0 1 2\n";
        const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("inf.ply"), text);
        QVERIFY2(r.doc, qPrintable(r.error));
        QCOMPARE(r.doc->stats.triangles, quint64(0));
    }
    {
        QElapsedTimer timer;
        timer.start();
        const QByteArray text = "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\n"
                                "property float z\nelement face 4000000000\nproperty list uchar int vertex_indices\n"
                                "end_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";
        const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("manyfaces.ply"), text);
        QVERIFY(!r.doc);
        QVERIFY(timer.elapsed() < 5000); // refused before reserving for the header's count
    }
    // A vertex count the file cannot hold is refused before allocating.
    {
        QByteArray b = "ply\nformat binary_little_endian 1.0\nelement vertex 50000000\n"
                       "property float x\nproperty float y\nproperty float z\nend_header\n";
        b += vertices;
        const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("shortvertices.ply"), b);
        QVERIFY(!r.doc);
        QVERIFY(!r.tooLarge);
    }
}

void TestModelLoaders::voxSelfReferencingGroupVisitsOnce()
{
    // Root transform 0 -> group 1 listing itself 64 times, the root (a cycle)
    // and transform 2 three times; transform 2 -> shape 3 -> model 0.
    QVector<int> children(64, 1);
    children << 0 << 2 << 2 << 2;
    const QByteArray single = voxFile(1, 1, 1, {{0, 0, 0, 1}});
    QByteArray b = single + voxTransform(0, 1) + voxGroup(1, children) + voxTransform(2, 3) + voxShape(3, 0);
    // voxFile's MAIN size covers its own children only: patch it.
    qToLittleEndian(quint32(b.size() - 20), b.data() + 16);
    QElapsedTimer timer;
    timer.start();
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("cycle.vox"), b);
    QVERIFY2(r.doc, qPrintable(r.error));
    QVERIFY(timer.elapsed() < 5000);
    QCOMPARE(r.doc->nodes.size(), size_t(3)); // transform 0, transform 2, one instance
    QCOMPARE(std::count_if(r.doc->nodes.begin(), r.doc->nodes.end(), [](const ModelNode &n) { return n.mesh >= 0; }),
             std::ptrdiff_t(1));
}

void TestModelLoaders::objManyUvsOnOnePositionWeldsFast()
{
    // One position with 100 000 distinct UVs: welding must not walk a chain
    // per corner (quadratic).
    constexpr int kUvs = 100000;
    QByteArray text = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
    text.reserve(kUvs * 40);
    for (int i = 0; i < kUvs; ++i)
        text += "vt " + QByteArray::number(i) + " 0\n";
    for (int i = 1; i <= kUvs; ++i)
        text += "f 1/" + QByteArray::number(i) + " 2/1 3/1\n";
    QElapsedTimer timer;
    timer.start();
    const ModelLoader::Result r = loadBytes(m_dir, QStringLiteral("uvs.obj"), text);
    QVERIFY2(r.doc, qPrintable(r.error));
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(r.doc->stats.triangles, quint64(kUvs));
    QCOMPARE(r.doc->stats.vertices, quint64(kUvs + 2)); // position 1 once per UV, 2 and 3 shared
}

QTEST_GUILESS_MAIN(TestModelLoaders)
#include "test_model_loaders.moc"
