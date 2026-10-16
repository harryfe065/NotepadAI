#ifndef MODELDOCUMENT_H
#define MODELDOCUMENT_H

#include <QImage>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QString>
#include <QStringList>
#include <QVector3D>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// A loaded 3D model, in one shared vertex / index buffer so the renderer
// uploads it with three buffer copies and draws every primitive from one VAO.
//
// Conventions every loader follows:
// - Right-handed, Y-up (Z-up sources set `zUp`; the viewer applies the root
//   rotation so the user can flip it), metres where the format says so.
// - UV origin at the top-left of the image (glTF); OBJ/FBX/PLY flip V.
// - Vertex colours are sRGB-encoded bytes (decoded in the shader).
// - Node parents come before their children.
//
// The loader fills it on a worker; the GUI thread then owns it. The renderer
// moves the heavy arrays (vertices, skin, indices, texture pixels) to the GPU
// and frees them, so after upload only the scene description stays in RAM.

// 28 bytes: position, normal packed as GL_INT_2_10_10_10_REV (snorm), UV,
// colour RGBA8 (sRGB).
struct ModelVertex
{
    float px = 0, py = 0, pz = 0;
    quint32 normal = 0;
    float u = 0, v = 0;
    quint32 color = 0xFFFFFFFFu;
};
static_assert(sizeof(ModelVertex) == 28, "ModelVertex layout is uploaded as is");

// 12 bytes, parallel to vertices when the document has skins. Weights sum to
// 255 (zero for vertices of unskinned primitives).
struct ModelSkinVertex
{
    quint16 joints[4] = {0, 0, 0, 0};
    quint8 weights[4] = {0, 0, 0, 0};
};
static_assert(sizeof(ModelSkinVertex) == 12, "ModelSkinVertex layout is uploaded as is");

enum class ModelTopology : quint8 { Triangles, Lines, Points };

struct ModelPrimitive
{
    // Indexed: indices[firstIndex, firstIndex + indexCount) + baseVertex.
    // Not indexed (indexCount == 0): vertices [baseVertex, baseVertex + vertexCount).
    quint64 firstIndex = 0;
    quint32 indexCount = 0;
    quint32 baseVertex = 0;
    quint32 vertexCount = 0;
    int material = -1;           // -1: default material
    int skin = -1;               // into skins; vertices then carry joints/weights
    ModelTopology topology = ModelTopology::Triangles;
    bool flatNormals = false;    // no normals in the file: the shader derives facet normals
    bool hasColor = false;
    QVector3D boundsMin, boundsMax; // mesh space (bind space when skinned)
};

struct ModelMesh
{
    QString name;
    int firstPrimitive = 0;
    int primitiveCount = 0;
};

struct ModelNode
{
    QString name;
    int parent = -1;
    QVector3D translation;
    QQuaternion rotation;
    QVector3D scale{1, 1, 1};
    bool hasMatrix = false;      // fixed local matrix instead of TRS (never animated)
    QMatrix4x4 matrix;
    int mesh = -1;
    bool visible = true;
};

struct ModelSkin
{
    QString name;
    std::vector<int> joints;                // node index per joint, -1 when missing
    std::vector<QMatrix4x4> inverseBind;    // per joint
};

enum class ModelAlphaMode : quint8 { Opaque, Mask, Blend };

struct ModelMaterial
{
    QString name;
    std::array<float, 4> baseColor{1, 1, 1, 1}; // linear
    std::array<float, 3> emissive{0, 0, 0};     // linear
    float metallic = 0.0f;
    float roughness = 0.6f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float alphaCutoff = 0.5f;
    int baseColorTexture = -1;
    int metallicTexture = -1;
    int roughnessTexture = -1;
    int normalTexture = -1;
    int occlusionTexture = -1;
    int emissiveTexture = -1;
    quint8 metallicChannel = 2;  // glTF packs metallic in B
    quint8 roughnessChannel = 1; // and roughness in G
    ModelAlphaMode alphaMode = ModelAlphaMode::Opaque;
    bool doubleSided = false;
    bool unlit = false;
    // UV transform (KHR_texture_transform / FBX), applied to every map.
    std::array<float, 2> uvOffset{0, 0};
    std::array<float, 2> uvScale{1, 1};
    float uvRotation = 0.0f;
};

enum class ModelWrap : quint8 { Repeat, Clamp, Mirror };

struct ModelTexture
{
    QString name;            // file name or "embedded #n", for the info line
    QImage image;            // RGBA8888 straight; null when it could not be loaded
    bool srgb = false;       // colour data (base colour, emissive)
    ModelWrap wrapS = ModelWrap::Repeat;
    ModelWrap wrapT = ModelWrap::Repeat;
    bool nearest = false;    // magnification without filtering (pixel art)
    bool mipmaps = true;
};

enum class ModelChannelPath : quint8 { Translation, Rotation, Scale };
enum class ModelInterpolation : quint8 { Step, Linear, CubicSpline };

struct ModelChannel
{
    int node = -1;
    ModelChannelPath path = ModelChannelPath::Translation;
    ModelInterpolation interpolation = ModelInterpolation::Linear;
    std::vector<float> times;    // seconds, ascending
    // 3 floats per key (T/S) or 4 (R, x y z w); cubic splines store
    // in-tangent, value, out-tangent per key.
    std::vector<float> values;
};

struct ModelAnimation
{
    QString name;
    float duration = 0.0f;
    std::vector<ModelChannel> channels;
};

struct ModelStats
{
    quint64 triangles = 0;
    quint64 lines = 0;
    quint64 points = 0;
    quint64 vertices = 0;
    int meshes = 0;
    int joints = 0;
};

struct ModelDocument
{
    QString format;          // "glTF 2.0 binary", "FBX 7.4 binary", "STL binary", ...
    QString detail;          // generator / exporter when known
    qint64 fileSize = 0;
    bool zUp = false;        // source is Z-up (STL, VOX): the viewer rotates the root by default
    bool cullBackFaces = false; // the format defines sidedness (glTF): cull unless doubleSided

    std::vector<ModelVertex> vertices;
    std::vector<ModelSkinVertex> skin; // empty, or one per vertex
    std::vector<quint32> indices;

    std::vector<ModelPrimitive> primitives;
    std::vector<ModelMesh> meshes;
    std::vector<ModelNode> nodes;
    std::vector<ModelSkin> skins;
    std::vector<ModelMaterial> materials;
    std::vector<ModelTexture> textures;
    std::vector<ModelAnimation> animations;

    QStringList warnings;    // shown under the info line ("2 textures not found")
    ModelStats stats;

    // Bytes the heavy arrays use (decode cap and upload progress).
    qint64 geometryBytes() const;
    qint64 textureBytes() const;
};

using ModelDocumentPtr = std::shared_ptr<ModelDocument>;

namespace ModelPack {

// Normal (any length) to GL_INT_2_10_10_10_REV snorm.
quint32 normal(float x, float y, float z);
// Linear [0,1] floats to sRGB-encoded RGBA8 (alpha stays linear).
quint32 colorFromLinear(float r, float g, float b, float a);
inline quint32 colorFromBytes(quint8 r, quint8 g, quint8 b, quint8 a)
{
    return quint32(r) | (quint32(g) << 8) | (quint32(b) << 16) | (quint32(a) << 24);
}
// [0,1] (float or double) to a rounded byte; NaN and negatives give 0.
template <typename T>
inline quint8 unorm8(T v)
{
    if (!(v > T(0))) return 0;
    if (v >= T(1)) return 255;
    return quint8(v * T(255) + T(0.5));
}
// sRGB-encoded component [0,1] to linear (material colours from FBX/OBJ/VOX).
float srgbToLinear(float c);
// Up to four (joint, weight) pairs, normalised so the bytes sum to 255.
ModelSkinVertex skinVertex(const quint32 *joints, const float *weights, int count);

} // namespace ModelPack

namespace ModelScene {

// Local matrix of a node in its rest pose.
QMatrix4x4 localMatrix(const ModelNode &node);
// World matrices of the rest pose, `root` applied above every root node.
std::vector<QMatrix4x4> restWorld(const ModelDocument &doc, const QMatrix4x4 &root);
// Root transform for a Z-up source (rotate -90 degrees about X).
QMatrix4x4 rootTransform(bool zUp);

// Fills each primitive's bounds (from the vertices it uses) and doc.stats.
// Run by the loader after the arrays are complete; parallel over primitives.
void finalize(ModelDocument &doc);

// World-space bounds of the drawn scene for the given world matrices.
// Skinned primitives use their bind-space bounds under `root` (their joint
// matrices already carry the node transforms). False when empty.
bool sceneBounds(const ModelDocument &doc, const std::vector<QMatrix4x4> &world, const QMatrix4x4 &root,
                 QVector3D &min, QVector3D &max);

} // namespace ModelScene

#endif // MODELDOCUMENT_H
