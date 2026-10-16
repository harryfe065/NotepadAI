#ifndef MODELRENDERER_H
#define MODELRENDERER_H

#include "ModelDocument.h"

#include <QColor>
#include <QCoreApplication>
#include <QMatrix4x4>
#include <QSize>
#include <QString>
#include <QVector3D>
#include <QVector4D>

#include <array>
#include <memory>
#include <vector>

class QElapsedTimer;
class QOpenGLContext;
class QOpenGLFunctions_3_3_Core;

// Draws a ModelDocument with OpenGL 3.3 core (4.1 core on macOS). All calls
// need the owning window's context current.
//
// Upload is progressive so a 1.5 GB model never stalls the GUI thread: each
// frame copies at most ~budget of geometry (16 MB glBufferSubData chunks),
// then texture rows (16 MB glTexSubImage2D bands). Nothing is drawn until the
// geometry is complete; textures show as the 1x1 default until theirs is.
// Each CPU array is freed as soon as it is on the GPU.
//
// Per frame: O(draw items) culling (one sphere transform each, skinned items
// skip it), state changes sorted by material (opaque, then alpha-tested),
// blended items sorted back to front (O(b log b), b = blended items only),
// one glDrawElementsBaseVertex per item. Joint matrices go to a texture
// buffer, re-uploaded only when the pose changed.
class ModelRenderer
{
    Q_DECLARE_TR_FUNCTIONS(ModelRenderer)

public:
    enum class Shading : quint8 { Shaded, Albedo, Normals, UvChecker };

    struct Settings {
        Shading shading = Shading::Shaded;
        bool wireframe = false;
        bool grid = true;
        bool skeleton = false;
    };

    struct Colors {
        QColor background;
        QColor gridMinor;
        QColor gridMajor;
        QColor wire;
        QColor skeleton;
    };

    struct Frame {
        QMatrix4x4 view;
        QMatrix4x4 projection;
        QVector3D eye;
        QVector3D target;
        QSize pixelSize;
        float devicePixelRatio = 1.0f;
        float sceneRadius = 1.0f;
        float groundY = 0.0f;
        const std::vector<QMatrix4x4> *world = nullptr;  // per node
        const std::vector<float> *joints = nullptr;      // 16 floats per joint, skins concatenated
        const std::vector<int> *skinBase = nullptr;      // first joint per skin
        bool jointsChanged = false;
    };

    ModelRenderer();
    ~ModelRenderer();
    ModelRenderer(const ModelRenderer &) = delete;
    ModelRenderer &operator=(const ModelRenderer &) = delete;

    // Resolves the functions, compiles the shaders. False with a user-facing
    // message when the context is below 3.3 core or a shader fails.
    bool initialize(QOpenGLContext *context, QString *error);
    bool isInitialized() const { return m_gl != nullptr; }
    // Deletes every GL object (context current). initialize() may follow.
    void release();

    // Replaces the drawn document. Its heavy arrays are uploaded by upload()
    // and freed from `doc` as they go.
    void setDocument(const ModelDocumentPtr &doc);
    void clearDocument();

    bool hasPendingUpload() const;
    bool geometryReady() const { return m_geometryDone; }
    // 0..1 over geometry and texture bytes.
    float uploadProgress() const;
    // Uploads for at most `budgetNs`; returns true while work remains.
    bool upload(qint64 budgetNs);
    // Set when the GPU refused an allocation (out of memory); the document
    // is then dropped and nothing is drawn.
    QString error() const { return m_error; }

    void render(const Frame &frame, const Settings &settings, const Colors &colors);

    // Unique joint nodes and their bone segments (child, parent joint), for
    // the skeleton overlay; computed once per document.
    bool hasSkeleton() const { return !m_jointNodes.empty(); }

    static constexpr qint64 kUploadBudgetNs = 8'000'000;

private:
    struct DrawItem {
        int node = -1;
        int primitive = -1;
        int material = -1;
        ModelAlphaMode alpha = ModelAlphaMode::Opaque;
        QVector3D center;    // primitive bounds, mesh space
        float radius = 0.0f;
        float depth = 0.0f;  // per frame, blended items
    };

    struct GpuTexture {
        unsigned id = 0;
        int nextRow = 0;
        bool ready = false;
    };

    struct Uniforms;
    struct Programs;

    bool buildPrograms(QString *error);
    void buildDrawItems();
    void buildSkeleton();
    void allocateGeometry();
    bool uploadGeometry(const QElapsedTimer &timer, qint64 budgetNs);
    bool uploadTextures(const QElapsedTimer &timer, qint64 budgetNs);
    void ensureJointBuffer(const Frame &frame);

    void bindMaterial(int material, bool unlitTopology);
    void drawItem(const DrawItem &item, const Frame &frame, int &boundKey, bool wirePass);
    void drawGrid(const Frame &frame, const Colors &colors);
    void drawSkeleton(const Frame &frame, const Colors &colors);
    unsigned textureId(int index) const;

    QOpenGLFunctions_3_3_Core *m_gl = nullptr;
    std::unique_ptr<Programs> m_programs;
    float m_maxAnisotropy = 1.0f;

    ModelDocumentPtr m_doc;
    unsigned m_vao = 0;
    unsigned m_emptyVao = 0;
    unsigned m_vbo = 0;
    unsigned m_skinVbo = 0;
    unsigned m_ebo = 0;
    unsigned m_jointTbo = 0;
    unsigned m_jointTex = 0;
    unsigned m_whiteTex = 0;
    unsigned m_lineVao = 0;
    unsigned m_lineVbo = 0;
    qint64 m_jointCapacity = 0; // floats

    // Upload progress.
    qint64 m_vertexBytes = 0, m_skinBytes = 0, m_indexBytes = 0;
    qint64 m_vertexDone = 0, m_skinDone = 0, m_indexDone = 0;
    qint64 m_textureBytes = 0, m_textureDone = 0;
    bool m_geometryDone = true;
    size_t m_nextTexture = 0;
    std::vector<GpuTexture> m_textures;

    std::vector<DrawItem> m_opaque;   // opaque then alpha-tested, by material
    std::vector<DrawItem> m_blended;
    std::vector<int> m_jointNodes;
    std::vector<std::pair<int, int>> m_bones; // indices into m_jointNodes
    std::vector<float> m_lineScratch;

    QMatrix4x4 m_viewProj;
    std::array<QVector4D, 6> m_planes;
    bool m_frontFaceCw = false;
    bool m_cullOn = false;
    bool m_jointsDirty = false;
    QString m_error;
};

#endif // MODELRENDERER_H
