#include "ModelRenderer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVersionFunctionsFactory>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

constexpr qint64 kUploadChunkBytes = qint64(16) << 20;
constexpr GLenum kTextureMaxAnisotropy = 0x84FE;    // GL_TEXTURE_MAX_ANISOTROPY(_EXT)
constexpr GLenum kMaxTextureMaxAnisotropy = 0x84FF; // GL_MAX_TEXTURE_MAX_ANISOTROPY(_EXT)
constexpr float kAnisotropy = 8.0f;

// Texture units of the main program.
enum Unit : std::uint8_t { BaseUnit, MetalUnit, RoughUnit, NormalUnit, OcclusionUnit, EmissiveUnit, JointUnit };

const char *kMainVertex = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec4 a_color;
layout(location = 4) in uvec4 a_joints;
layout(location = 5) in vec4 a_weights;

uniform mat4 u_viewProj;
uniform mat4 u_model;
uniform mat3 u_normalMatrix;
uniform bool u_skinned;
uniform int u_jointBase;
uniform samplerBuffer u_joints;
uniform vec3 u_uvRow0;
uniform vec3 u_uvRow1;
uniform float u_pointSize;

out vec3 v_world;
out vec3 v_normal;
out vec2 v_uv;
out vec2 v_rawUv;
out vec4 v_color;

mat4 joint(uint j)
{
    int b = (u_jointBase + int(j)) * 4;
    return mat4(texelFetch(u_joints, b), texelFetch(u_joints, b + 1),
                texelFetch(u_joints, b + 2), texelFetch(u_joints, b + 3));
}

vec3 srgbToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

void main()
{
    vec4 p = vec4(a_pos, 1.0);
    vec3 n = a_normal.xyz;
    if (u_skinned) {
        mat4 s = a_weights.x * joint(a_joints.x) + a_weights.y * joint(a_joints.y)
               + a_weights.z * joint(a_joints.z) + a_weights.w * joint(a_joints.w);
        p = s * p;
        n = mat3(s) * n;
    } else {
        p = u_model * p;
        n = u_normalMatrix * n;
    }
    v_world = p.xyz;
    v_normal = n;
    vec3 uv1 = vec3(a_uv, 1.0);
    v_uv = vec2(dot(u_uvRow0, uv1), dot(u_uvRow1, uv1));
    v_rawUv = a_uv;
    v_color = vec4(srgbToLinear(a_color.rgb), a_color.a);
    gl_Position = u_viewProj * p;
    gl_PointSize = u_pointSize;
}
)";

const char *kMainFragment = R"(#version 330 core
in vec3 v_world;
in vec3 v_normal;
in vec2 v_uv;
in vec2 v_rawUv;
in vec4 v_color;
out vec4 o_color;

uniform int u_mode;          // 0 shaded, 1 albedo, 2 normals, 3 UV checker, 4 flat colour
uniform vec4 u_baseColor;
uniform vec3 u_emissive;
uniform float u_metallic;
uniform float u_roughness;
uniform float u_normalScale;
uniform float u_occlusion;
uniform float u_alphaCutoff;
uniform int u_alphaMode;     // 0 opaque, 1 mask, 2 blend
uniform bool u_unlit;
uniform bool u_flat;
uniform bool u_hasNormalMap;
uniform ivec2 u_mrChannels;
uniform sampler2D u_baseTex;
uniform sampler2D u_metalTex;
uniform sampler2D u_roughTex;
uniform sampler2D u_normalTex;
uniform sampler2D u_occTex;
uniform sampler2D u_emisTex;
uniform vec3 u_eye;
uniform vec3 u_lightDir[2];
uniform vec3 u_lightColor[2];
uniform vec3 u_skyZenith;
uniform vec3 u_skyHorizon;
uniform vec3 u_skyGround;
uniform vec4 u_wireColor;

const float PI = 3.14159265;

vec3 linearToSrgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// Procedural studio environment: a vertical gradient, world-fixed.
vec3 sky(vec3 d)
{
    return d.y >= 0.0 ? mix(u_skyHorizon, u_skyZenith, sqrt(d.y))
                      : mix(u_skyHorizon, u_skyGround, sqrt(min(-d.y * 2.5, 1.0)));
}

vec3 irradiance(vec3 n)
{
    return mix(u_skyGround, u_skyZenith, n.y * 0.5 + 0.5) * 0.75 + u_skyHorizon * (0.25 * (1.0 - abs(n.y)));
}

vec3 envSpecular(vec3 r, float rough)
{
    return mix(sky(r), irradiance(r), clamp(rough * 1.3, 0.0, 1.0));
}

// Karis' analytic fit of the split-sum BRDF.
vec3 envBrdf(vec3 f0, float rough, float nv)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// Tangent frame from screen-space derivatives (no tangent attribute needed).
mat3 cotangentFrame(vec3 n, vec3 p, vec2 uv)
{
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, n);
    vec3 dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float m = max(dot(t, t), dot(b, b));
    float inv = m > 0.0 ? inversesqrt(m) : 0.0;
    // V grows downwards in the image (glTF), so the map's +Y is -dP/dv.
    return mat3(t * inv, -b * inv, n);
}

float channel(vec4 t, int c)
{
    return c == 0 ? t.r : c == 1 ? t.g : c == 2 ? t.b : t.a;
}

void main()
{
    if (u_mode == 4) {
        o_color = u_wireColor;
        return;
    }
    vec4 base = u_baseColor * v_color * texture(u_baseTex, v_uv);
    if (u_alphaMode == 1 && base.a < u_alphaCutoff)
        discard;
    float alpha = u_alphaMode == 2 ? base.a : 1.0;

    vec3 facet = normalize(cross(dFdx(v_world), dFdy(v_world))); // faces the viewer
    vec3 n = facet;
    float len = length(v_normal);
    if (!u_flat && len > 1e-8) {
        n = v_normal / len;
        if (!gl_FrontFacing)
            n = -n;
    }
    if (u_hasNormalMap) {
        vec3 tn = texture(u_normalTex, v_uv).xyz * 2.0 - 1.0;
        tn.xy *= u_normalScale;
        vec3 mapped = cotangentFrame(n, v_world, v_uv) * tn;
        if (dot(mapped, mapped) > 1e-12)
            n = normalize(mapped);
    }

    if (u_mode == 1) {
        o_color = vec4(linearToSrgb(base.rgb), alpha);
        return;
    }
    if (u_mode == 2) {
        o_color = vec4(n * 0.5 + 0.5, alpha);
        return;
    }
    vec3 v = normalize(u_eye - v_world);
    if (u_mode == 3) {
        vec2 cell = floor(v_rawUv * 8.0);
        vec2 f = fract(v_rawUv);
        vec3 tint = vec3(0.55 + 0.45 * f.x, 0.55 + 0.45 * f.y, 0.75);
        vec3 c = mix(vec3(0.18), vec3(0.92), mod(cell.x + cell.y, 2.0)) * tint;
        o_color = vec4(c * (0.35 + 0.65 * abs(dot(n, v))), 1.0);
        return;
    }
    if (u_unlit) {
        o_color = vec4(linearToSrgb(base.rgb), alpha);
        return;
    }

    float metallic = clamp(u_metallic * channel(texture(u_metalTex, v_uv), u_mrChannels.x), 0.0, 1.0);
    float rough = clamp(u_roughness * channel(texture(u_roughTex, v_uv), u_mrChannels.y), 0.045, 1.0);
    float ao = 1.0 + u_occlusion * (texture(u_occTex, v_uv).r - 1.0);
    vec3 emissive = u_emissive * texture(u_emisTex, v_uv).rgb;

    float nv = max(dot(n, v), 1e-4);
    vec3 f0 = mix(vec3(0.04), base.rgb, metallic);
    vec3 diffuse = base.rgb * (1.0 - metallic);
    float a = rough * rough;
    float a2 = a * a;
    vec3 color = vec3(0.0);
    for (int i = 0; i < 2; ++i) {
        vec3 l = u_lightDir[i];
        float nl = dot(n, l);
        if (nl <= 0.0)
            continue;
        vec3 h = normalize(l + v);
        float nh = max(dot(n, h), 0.0);
        float vh = max(dot(v, h), 0.0);
        float d = nh * nh * (a2 - 1.0) + 1.0;
        float D = a2 / (PI * d * d);
        float vis = 0.5 / (nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2));
        vec3 F = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
        color += ((1.0 - F) * diffuse / PI + D * vis * F) * u_lightColor[i] * nl;
    }
    vec3 r = reflect(-v, n);
    color += (diffuse * irradiance(n) + envBrdf(f0, rough, nv) * envSpecular(r, rough)) * ao;
    color += emissive;
    o_color = vec4(linearToSrgb(aces(color)), alpha);
}
)";

// Fullscreen triangle; the fragment shader intersects each pixel's ray with
// the ground plane, so the grid is infinite and costs one triangle.
const char *kGridVertex = R"(#version 330 core
out vec2 v_ndc;
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_ndc = p * 2.0 - 1.0;
    gl_Position = vec4(v_ndc, 0.0, 1.0);
}
)";

const char *kGridFragment = R"(#version 330 core
in vec2 v_ndc;
out vec4 o_color;

uniform mat4 u_invViewProj;
uniform mat4 u_viewProj;
uniform float u_groundY;
uniform float u_cell;
uniform vec3 u_center;
uniform float u_fadeNear;
uniform float u_fadeFar;
uniform vec4 u_minor;
uniform vec4 u_major;
uniform vec4 u_xAxis;
uniform vec4 u_zAxis;

// Anti-aliased 1px lines on integer coordinates; `density` is how many
// cells one pixel covers (lines fade out before they turn into a solid).
float lineMask(vec2 coord, out float density)
{
    vec2 w = fwidth(coord);
    density = max(w.x, w.y);
    vec2 a = abs(fract(coord - 0.5) - 0.5) / max(w, vec2(1e-6));
    return 1.0 - min(min(a.x, a.y), 1.0);
}

void main()
{
    vec4 pn = u_invViewProj * vec4(v_ndc, -1.0, 1.0);
    vec4 pf = u_invViewProj * vec4(v_ndc, 1.0, 1.0);
    vec3 a = pn.xyz / pn.w;
    vec3 b = pf.xyz / pf.w;
    float dy = b.y - a.y;
    if (abs(dy) < 1e-20)
        discard;
    float t = (u_groundY - a.y) / dy;
    if (t < 0.0 || t > 1.0)
        discard;
    vec3 p = mix(a, b, t);

    float dMinor;
    float dMajor;
    float minor = lineMask(p.xz / u_cell, dMinor) * (1.0 - smoothstep(0.25, 0.5, dMinor));
    float major = lineMask(p.xz / (u_cell * 10.0), dMajor) * (1.0 - smoothstep(0.25, 0.5, dMajor));
    vec2 wa = max(fwidth(p.xz), vec2(1e-6));
    float xAxis = 1.0 - min(abs(p.z) / wa.y, 1.0);
    float zAxis = 1.0 - min(abs(p.x) / wa.x, 1.0);

    vec4 c = vec4(u_minor.rgb, u_minor.a * minor);
    c = mix(c, u_major, major);
    c = mix(c, u_xAxis, xAxis);
    c = mix(c, u_zAxis, zAxis);
    c.a *= 1.0 - smoothstep(u_fadeNear, u_fadeFar, length(p.xz - u_center.xz));
    if (c.a < 0.004)
        discard;
    vec4 clip = u_viewProj * vec4(p, 1.0);
    gl_FragDepth = clip.z / clip.w * 0.5 + 0.5;
    o_color = c;
}
)";

const char *kLineVertex = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
uniform mat4 u_viewProj;
uniform float u_pointSize;
void main()
{
    gl_Position = u_viewProj * vec4(a_pos, 1.0);
    gl_PointSize = u_pointSize;
}
)";

const char *kLineFragment = R"(#version 330 core
uniform vec4 u_color;
uniform bool u_round;
out vec4 o_color;
void main()
{
    if (u_round) {
        vec2 d = gl_PointCoord * 2.0 - 1.0;
        if (dot(d, d) > 1.0)
            discard;
    }
    o_color = u_color;
}
)";

GLint wrapMode(ModelWrap wrap)
{
    switch (wrap) {
    case ModelWrap::Clamp: return GL_CLAMP_TO_EDGE;
    case ModelWrap::Mirror: return GL_MIRRORED_REPEAT;
    case ModelWrap::Repeat: break;
    }
    return GL_REPEAT;
}

const ModelMaterial &defaultMaterial()
{
    static const ModelMaterial material = [] {
        ModelMaterial m;
        m.baseColor = {0.7f, 0.7f, 0.7f, 1.0f};
        m.doubleSided = true;
        return m;
    }();
    return material;
}

template<typename T>
void freeVector(std::vector<T> &v)
{
    std::vector<T>().swap(v);
}

} // namespace

struct ModelRenderer::Uniforms
{
    GLint viewProj, model, normalMatrix, skinned, jointBase, uvRow0, uvRow1, pointSize;
    GLint mode, baseColor, emissive, metallic, roughness, normalScale, occlusion, alphaCutoff, alphaMode;
    GLint unlit, flat, hasNormalMap, mrChannels, eye, lightDir, lightColor;
    GLint skyZenith, skyHorizon, skyGround, wireColor;
};

struct ModelRenderer::Programs
{
    QOpenGLShaderProgram main;
    QOpenGLShaderProgram grid;
    QOpenGLShaderProgram line;
    Uniforms u{};
    struct {
        GLint invViewProj, viewProj, groundY, cell, center, fadeNear, fadeFar, minor, major, xAxis, zAxis;
    } g{};
    struct {
        GLint viewProj, pointSize, color, round;
    } l{};
};

ModelRenderer::ModelRenderer() = default;
ModelRenderer::~ModelRenderer() = default; // release() must have run with the context current

bool ModelRenderer::initialize(QOpenGLContext *context, QString *error)
{
    const QSurfaceFormat format = context->format();
    if (context->isOpenGLES() || format.version() < qMakePair(3, 3)) {
        *error = tr("3D preview needs OpenGL 3.3; this system provides %1.%2.")
                     .arg(format.majorVersion())
                     .arg(format.minorVersion());
        return false;
    }
    auto *gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context);
    if (!gl || !gl->initializeOpenGLFunctions()) {
        *error = tr("3D preview could not load the OpenGL 3.3 functions.");
        return false;
    }
    m_gl = gl;
    if (!buildPrograms(error)) {
        release();
        return false;
    }

    m_maxAnisotropy = 1.0f;
    if (context->hasExtension(QByteArrayLiteral("GL_EXT_texture_filter_anisotropic"))
        || context->hasExtension(QByteArrayLiteral("GL_ARB_texture_filter_anisotropic"))) {
        GLfloat max = 1.0f;
        m_gl->glGetFloatv(kMaxTextureMaxAnisotropy, &max);
        m_maxAnisotropy = std::min(kAnisotropy, std::max(1.0f, max));
    }

    const quint32 white = 0xFFFFFFFFu;
    m_gl->glGenTextures(1, &m_whiteTex);
    m_gl->glBindTexture(GL_TEXTURE_2D, m_whiteTex);
    m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    m_gl->glGenVertexArrays(1, &m_emptyVao);
    m_gl->glGenVertexArrays(1, &m_lineVao);
    m_gl->glGenBuffers(1, &m_lineVbo);
    m_gl->glBindVertexArray(m_lineVao);
    m_gl->glBindBuffer(GL_ARRAY_BUFFER, m_lineVbo);
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, nullptr);
    m_gl->glBindVertexArray(0);

    m_gl->glGenBuffers(1, &m_jointTbo);
    m_gl->glGenTextures(1, &m_jointTex);
    m_jointCapacity = 0;

    m_gl->glEnable(GL_PROGRAM_POINT_SIZE);
    return true;
}

bool ModelRenderer::buildPrograms(QString *error)
{
    m_programs = std::make_unique<Programs>();
    auto build = [error](QOpenGLShaderProgram &program, const char *vs, const char *fs) {
        if (program.addShaderFromSourceCode(QOpenGLShader::Vertex, vs)
            && program.addShaderFromSourceCode(QOpenGLShader::Fragment, fs) && program.link())
            return true;
        *error = tr("3D preview could not compile its shaders: %1").arg(program.log().trimmed());
        return false;
    };
    Programs &p = *m_programs;
    if (!build(p.main, kMainVertex, kMainFragment) || !build(p.grid, kGridVertex, kGridFragment)
        || !build(p.line, kLineVertex, kLineFragment))
        return false;

    auto loc = [](QOpenGLShaderProgram &program, const char *name) { return program.uniformLocation(name); };
    Uniforms &u = p.u;
    u.viewProj = loc(p.main, "u_viewProj");
    u.model = loc(p.main, "u_model");
    u.normalMatrix = loc(p.main, "u_normalMatrix");
    u.skinned = loc(p.main, "u_skinned");
    u.jointBase = loc(p.main, "u_jointBase");
    u.uvRow0 = loc(p.main, "u_uvRow0");
    u.uvRow1 = loc(p.main, "u_uvRow1");
    u.pointSize = loc(p.main, "u_pointSize");
    u.mode = loc(p.main, "u_mode");
    u.baseColor = loc(p.main, "u_baseColor");
    u.emissive = loc(p.main, "u_emissive");
    u.metallic = loc(p.main, "u_metallic");
    u.roughness = loc(p.main, "u_roughness");
    u.normalScale = loc(p.main, "u_normalScale");
    u.occlusion = loc(p.main, "u_occlusion");
    u.alphaCutoff = loc(p.main, "u_alphaCutoff");
    u.alphaMode = loc(p.main, "u_alphaMode");
    u.unlit = loc(p.main, "u_unlit");
    u.flat = loc(p.main, "u_flat");
    u.hasNormalMap = loc(p.main, "u_hasNormalMap");
    u.mrChannels = loc(p.main, "u_mrChannels");
    u.eye = loc(p.main, "u_eye");
    u.lightDir = loc(p.main, "u_lightDir");
    u.lightColor = loc(p.main, "u_lightColor");
    u.skyZenith = loc(p.main, "u_skyZenith");
    u.skyHorizon = loc(p.main, "u_skyHorizon");
    u.skyGround = loc(p.main, "u_skyGround");
    u.wireColor = loc(p.main, "u_wireColor");

    p.main.bind();
    const std::pair<const char *, int> samplers[] = {
        {"u_baseTex", BaseUnit},  {"u_metalTex", MetalUnit},    {"u_roughTex", RoughUnit}, {"u_normalTex", NormalUnit},
        {"u_occTex", OcclusionUnit}, {"u_emisTex", EmissiveUnit}, {"u_joints", JointUnit}};
    for (const auto &[name, unit] : samplers)
        m_gl->glUniform1i(loc(p.main, name), unit);
    p.main.release();

    p.g.invViewProj = loc(p.grid, "u_invViewProj");
    p.g.viewProj = loc(p.grid, "u_viewProj");
    p.g.groundY = loc(p.grid, "u_groundY");
    p.g.cell = loc(p.grid, "u_cell");
    p.g.center = loc(p.grid, "u_center");
    p.g.fadeNear = loc(p.grid, "u_fadeNear");
    p.g.fadeFar = loc(p.grid, "u_fadeFar");
    p.g.minor = loc(p.grid, "u_minor");
    p.g.major = loc(p.grid, "u_major");
    p.g.xAxis = loc(p.grid, "u_xAxis");
    p.g.zAxis = loc(p.grid, "u_zAxis");

    p.l.viewProj = loc(p.line, "u_viewProj");
    p.l.pointSize = loc(p.line, "u_pointSize");
    p.l.color = loc(p.line, "u_color");
    p.l.round = loc(p.line, "u_round");
    return true;
}

void ModelRenderer::release()
{
    if (!m_gl) {
        m_programs.reset();
        return;
    }
    clearDocument();
    auto deleteBuffer = [this](unsigned &id) {
        if (id) m_gl->glDeleteBuffers(1, &id);
        id = 0;
    };
    auto deleteTexture = [this](unsigned &id) {
        if (id) m_gl->glDeleteTextures(1, &id);
        id = 0;
    };
    auto deleteVao = [this](unsigned &id) {
        if (id) m_gl->glDeleteVertexArrays(1, &id);
        id = 0;
    };
    deleteBuffer(m_lineVbo);
    deleteBuffer(m_jointTbo);
    deleteTexture(m_jointTex);
    deleteTexture(m_whiteTex);
    deleteVao(m_lineVao);
    deleteVao(m_emptyVao);
    m_jointCapacity = 0;
    m_programs.reset();
    m_gl = nullptr;
}

void ModelRenderer::clearDocument()
{
    if (m_gl) {
        if (m_vao) m_gl->glDeleteVertexArrays(1, &m_vao);
        for (unsigned *b : {&m_vbo, &m_skinVbo, &m_ebo})
            if (*b) m_gl->glDeleteBuffers(1, b);
        for (GpuTexture &t : m_textures)
            if (t.id) m_gl->glDeleteTextures(1, &t.id);
    }
    m_vao = m_vbo = m_skinVbo = m_ebo = 0;
    m_textures.clear();
    m_doc.reset();
    m_opaque.clear();
    m_blended.clear();
    m_jointNodes.clear();
    m_bones.clear();
    m_vertexBytes = m_skinBytes = m_indexBytes = 0;
    m_vertexDone = m_skinDone = m_indexDone = 0;
    m_textureBytes = m_textureDone = 0;
    m_nextTexture = 0;
    m_geometryDone = true;
    m_error.clear();
}

void ModelRenderer::setDocument(const ModelDocumentPtr &doc)
{
    clearDocument();
    if (!doc || !m_gl)
        return;
    m_doc = doc;
    m_vertexBytes = qint64(doc->vertices.size() * sizeof(ModelVertex));
    m_skinBytes = qint64(doc->skin.size() * sizeof(ModelSkinVertex));
    m_indexBytes = qint64(doc->indices.size() * sizeof(quint32));
    m_textures.resize(doc->textures.size());
    for (const ModelTexture &t : doc->textures)
        m_textureBytes += t.image.isNull() ? 0 : qint64(t.image.height()) * t.image.bytesPerLine();
    m_geometryDone = false;
    m_jointsDirty = true;
    allocateGeometry();
    buildDrawItems();
    buildSkeleton();
}

void ModelRenderer::allocateGeometry()
{
    QOpenGLFunctions_3_3_Core &gl = *m_gl;
    while (gl.glGetError() != GL_NO_ERROR) {
    }
    gl.glGenVertexArrays(1, &m_vao);
    gl.glBindVertexArray(m_vao);

    if (m_vertexBytes > 0) {
        gl.glGenBuffers(1, &m_vbo);
        gl.glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        gl.glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(m_vertexBytes), nullptr, GL_STATIC_DRAW);
        const GLsizei stride = sizeof(ModelVertex);
        gl.glEnableVertexAttribArray(0);
        gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(ModelVertex, px)));
        gl.glEnableVertexAttribArray(1);
        gl.glVertexAttribPointer(1, 4, GL_INT_2_10_10_10_REV, GL_TRUE, stride,
                                 reinterpret_cast<void *>(offsetof(ModelVertex, normal)));
        gl.glEnableVertexAttribArray(2);
        gl.glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(ModelVertex, u)));
        gl.glEnableVertexAttribArray(3);
        gl.glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                                 reinterpret_cast<void *>(offsetof(ModelVertex, color)));
    }
    if (m_skinBytes > 0) {
        gl.glGenBuffers(1, &m_skinVbo);
        gl.glBindBuffer(GL_ARRAY_BUFFER, m_skinVbo);
        gl.glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(m_skinBytes), nullptr, GL_STATIC_DRAW);
        const GLsizei stride = sizeof(ModelSkinVertex);
        gl.glEnableVertexAttribArray(4);
        gl.glVertexAttribIPointer(4, 4, GL_UNSIGNED_SHORT, stride,
                                  reinterpret_cast<void *>(offsetof(ModelSkinVertex, joints)));
        gl.glEnableVertexAttribArray(5);
        gl.glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                                 reinterpret_cast<void *>(offsetof(ModelSkinVertex, weights)));
    }
    if (m_indexBytes > 0) {
        gl.glGenBuffers(1, &m_ebo);
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
        gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(m_indexBytes), nullptr, GL_STATIC_DRAW);
    }
    gl.glBindVertexArray(0);
    gl.glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (gl.glGetError() == GL_OUT_OF_MEMORY) {
        const qint64 total = m_vertexBytes + m_skinBytes + m_indexBytes;
        clearDocument();
        m_error = tr("The graphics card has no room for this model's %1 MB of geometry.").arg(total >> 20);
    }
}

void ModelRenderer::buildDrawItems()
{
    const ModelDocument &doc = *m_doc;
    for (size_t n = 0; n < doc.nodes.size(); ++n) {
        const ModelNode &node = doc.nodes[n];
        if (node.mesh < 0 || size_t(node.mesh) >= doc.meshes.size() || !node.visible)
            continue;
        const ModelMesh &mesh = doc.meshes[size_t(node.mesh)];
        for (int p = mesh.firstPrimitive; p < mesh.firstPrimitive + mesh.primitiveCount; ++p) {
            if (p < 0 || size_t(p) >= doc.primitives.size())
                continue;
            const ModelPrimitive &prim = doc.primitives[size_t(p)];
            if (prim.indexCount == 0 && prim.vertexCount == 0)
                continue;
            DrawItem item;
            item.node = int(n);
            item.primitive = p;
            item.material = prim.material >= 0 && size_t(prim.material) < doc.materials.size() ? prim.material : -1;
            item.alpha = item.material >= 0 ? doc.materials[size_t(item.material)].alphaMode : ModelAlphaMode::Opaque;
            item.center = (prim.boundsMin + prim.boundsMax) * 0.5f;
            item.radius = (prim.boundsMax - prim.boundsMin).length() * 0.5f;
            (item.alpha == ModelAlphaMode::Blend ? m_blended : m_opaque).push_back(item);
        }
    }
    // Opaque first, alpha-tested after (early-z keeps working for the bulk),
    // grouped by material so state changes are rare.
    std::stable_sort(m_opaque.begin(), m_opaque.end(), [](const DrawItem &a, const DrawItem &b) {
        if (a.alpha != b.alpha)
            return a.alpha < b.alpha;
        return a.material < b.material;
    });
}

void ModelRenderer::buildSkeleton()
{
    const ModelDocument &doc = *m_doc;
    std::vector<int> slot(doc.nodes.size(), -1);
    for (const ModelSkin &skin : doc.skins) {
        for (const int j : skin.joints) {
            if (j >= 0 && size_t(j) < slot.size() && slot[size_t(j)] < 0) {
                slot[size_t(j)] = int(m_jointNodes.size());
                m_jointNodes.push_back(j);
            }
        }
    }
    for (size_t k = 0; k < m_jointNodes.size(); ++k) {
        int p = doc.nodes[size_t(m_jointNodes[k])].parent;
        while (p >= 0 && slot[size_t(p)] < 0)
            p = doc.nodes[size_t(p)].parent;
        if (p >= 0)
            m_bones.emplace_back(int(k), slot[size_t(p)]);
    }
}

bool ModelRenderer::hasPendingUpload() const
{
    return m_doc && (!m_geometryDone || m_nextTexture < m_textures.size());
}

float ModelRenderer::uploadProgress() const
{
    const qint64 total = m_vertexBytes + m_skinBytes + m_indexBytes + m_textureBytes;
    const qint64 done = m_vertexDone + m_skinDone + m_indexDone + m_textureDone;
    return total > 0 ? float(double(done) / double(total)) : 1.0f;
}

bool ModelRenderer::upload(qint64 budgetNs)
{
    if (!m_doc || !m_gl)
        return false;
    QElapsedTimer timer;
    timer.start();
    if (!m_geometryDone) {
        if (!uploadGeometry(timer, budgetNs))
            return true;
        m_geometryDone = true;
    }
    return uploadTextures(timer, budgetNs);
}

bool ModelRenderer::uploadGeometry(const QElapsedTimer &timer, qint64 budgetNs)
{
    QOpenGLFunctions_3_3_Core &gl = *m_gl;
    // The element buffer binding is VAO state; keep ours bound for it.
    gl.glBindVertexArray(m_vao);
    auto copy = [&](GLenum target, unsigned buffer, const void *data, qint64 total, qint64 &done) {
        if (done >= total)
            return true;
        gl.glBindBuffer(target, buffer);
        while (done < total) {
            if (timer.nsecsElapsed() >= budgetNs)
                return false;
            const qint64 n = std::min(kUploadChunkBytes, total - done);
            gl.glBufferSubData(target, GLintptr(done), GLsizeiptr(n), static_cast<const char *>(data) + done);
            done += n;
        }
        return true;
    };
    ModelDocument &doc = *m_doc;
    bool complete = copy(GL_ARRAY_BUFFER, m_vbo, doc.vertices.data(), m_vertexBytes, m_vertexDone);
    if (complete)
        freeVector(doc.vertices);
    complete = complete && copy(GL_ARRAY_BUFFER, m_skinVbo, doc.skin.data(), m_skinBytes, m_skinDone);
    if (complete)
        freeVector(doc.skin);
    complete = complete && copy(GL_ELEMENT_ARRAY_BUFFER, m_ebo, doc.indices.data(), m_indexBytes, m_indexDone);
    if (complete)
        freeVector(doc.indices);
    gl.glBindVertexArray(0);
    gl.glBindBuffer(GL_ARRAY_BUFFER, 0);
    return complete;
}

bool ModelRenderer::uploadTextures(const QElapsedTimer &timer, qint64 budgetNs)
{
    QOpenGLFunctions_3_3_Core &gl = *m_gl;
    ModelDocument &doc = *m_doc;
    while (m_nextTexture < m_textures.size()) {
        if (timer.nsecsElapsed() >= budgetNs)
            return true;
        ModelTexture &src = doc.textures[m_nextTexture];
        GpuTexture &dst = m_textures[m_nextTexture];
        if (src.image.isNull()) {
            ++m_nextTexture;
            continue;
        }
        if (src.image.format() != QImage::Format_RGBA8888)
            src.image.convertTo(QImage::Format_RGBA8888);
        const int w = src.image.width();
        const int h = src.image.height();
        const qsizetype stride = src.image.bytesPerLine();
        if (!dst.id) {
            gl.glGenTextures(1, &dst.id);
            gl.glBindTexture(GL_TEXTURE_2D, dst.id);
            gl.glTexImage2D(GL_TEXTURE_2D, 0, src.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, w, h, 0, GL_RGBA,
                            GL_UNSIGNED_BYTE, nullptr);
        }
        gl.glBindTexture(GL_TEXTURE_2D, dst.id);
        gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(stride / 4));
        const int rows = int(std::clamp<qint64>(kUploadChunkBytes / std::max<qint64>(stride, 1), 1, h));
        while (dst.nextRow < h && timer.nsecsElapsed() < budgetNs) {
            const int n = std::min(rows, h - dst.nextRow);
            gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dst.nextRow, w, n, GL_RGBA, GL_UNSIGNED_BYTE,
                               src.image.constScanLine(dst.nextRow));
            dst.nextRow += n;
            m_textureDone += qint64(n) * stride;
        }
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (dst.nextRow < h)
            return true;

        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode(src.wrapS));
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode(src.wrapT));
        const GLint minFilter = src.mipmaps ? (src.nearest ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR)
                                            : (src.nearest ? GL_NEAREST : GL_LINEAR);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, src.nearest ? GL_NEAREST : GL_LINEAR);
        if (src.mipmaps) {
            gl.glGenerateMipmap(GL_TEXTURE_2D);
            if (m_maxAnisotropy > 1.0f && !src.nearest)
                gl.glTexParameterf(GL_TEXTURE_2D, kTextureMaxAnisotropy, m_maxAnisotropy);
        }
        dst.ready = true;
        src.image = QImage();
        ++m_nextTexture;
    }
    gl.glBindTexture(GL_TEXTURE_2D, 0);
    return false;
}

unsigned ModelRenderer::textureId(int index) const
{
    if (index < 0 || size_t(index) >= m_textures.size())
        return 0;
    const GpuTexture &t = m_textures[size_t(index)];
    return t.ready ? t.id : 0;
}

void ModelRenderer::ensureJointBuffer(const Frame &frame)
{
    if (!frame.joints || frame.joints->empty() || (!frame.jointsChanged && !m_jointsDirty))
        return;
    QOpenGLFunctions_3_3_Core &gl = *m_gl;
    const qint64 floats = qint64(frame.joints->size());
    gl.glBindBuffer(GL_TEXTURE_BUFFER, m_jointTbo);
    // Orphan and refill: the driver hands back fresh storage instead of
    // waiting for the previous frame's draws to finish reading.
    gl.glBufferData(GL_TEXTURE_BUFFER, GLsizeiptr(floats * 4), frame.joints->data(), GL_STREAM_DRAW);
    if (floats != m_jointCapacity) {
        gl.glBindTexture(GL_TEXTURE_BUFFER, m_jointTex);
        gl.glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, m_jointTbo);
        m_jointCapacity = floats;
    }
    gl.glBindBuffer(GL_TEXTURE_BUFFER, 0);
    m_jointsDirty = false;
}

void ModelRenderer::bindMaterial(int material, bool unlitTopology)
{
    QOpenGLFunctions_3_3_Core &gl = *m_gl;
    const Uniforms &u = m_programs->u;
    const ModelMaterial &m = material >= 0 ? m_doc->materials[size_t(material)] : defaultMaterial();
    gl.glUniform4f(u.baseColor, m.baseColor[0], m.baseColor[1], m.baseColor[2], m.baseColor[3]);
    gl.glUniform3f(u.emissive, m.emissive[0], m.emissive[1], m.emissive[2]);
    gl.glUniform1f(u.metallic, m.metallic);
    gl.glUniform1f(u.roughness, m.roughness);
    gl.glUniform1f(u.normalScale, m.normalScale);
    gl.glUniform1f(u.occlusion, m.occlusionStrength);
    gl.glUniform1f(u.alphaCutoff, m.alphaCutoff);
    gl.glUniform1i(u.alphaMode, int(m.alphaMode));
    gl.glUniform1i(u.unlit, m.unlit || unlitTopology);
    gl.glUniform2i(u.mrChannels, m.metallicChannel, m.roughnessChannel);

    // KHR_texture_transform order: translate * rotate * scale.
    const float c = std::cos(m.uvRotation);
    const float s = std::sin(m.uvRotation);
    const float sx = m.uvScale[0];
    const float sy = m.uvScale[1];
    gl.glUniform3f(u.uvRow0, c * sx, s * sy, m.uvOffset[0]);
    gl.glUniform3f(u.uvRow1, -s * sx, c * sy, m.uvOffset[1]);

    const std::pair<int, int> maps[] = {{BaseUnit, m.baseColorTexture},   {MetalUnit, m.metallicTexture},
                                        {RoughUnit, m.roughnessTexture},  {NormalUnit, m.normalTexture},
                                        {OcclusionUnit, m.occlusionTexture}, {EmissiveUnit, m.emissiveTexture}};
    for (const auto &[unit, index] : maps) {
        const unsigned id = textureId(index);
        gl.glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        gl.glBindTexture(GL_TEXTURE_2D, id ? id : m_whiteTex);
    }
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glUniform1i(u.hasNormalMap, textureId(m.normalTexture) != 0);

    const bool cull = m_doc->cullBackFaces && !m.doubleSided;
    if (cull != m_cullOn) {
        cull ? gl.glEnable(GL_CULL_FACE) : gl.glDisable(GL_CULL_FACE);
        m_cullOn = cull;
    }
}

void ModelRenderer::drawItem(const DrawItem &item, const Frame &frame, int &boundKey, bool wirePass)
{
    QOpenGLFunctions_3_3_Core &gl = *m_gl;
    const Uniforms &u = m_programs->u;
    const ModelPrimitive &prim = m_doc->primitives[size_t(item.primitive)];
    if (wirePass && prim.topology != ModelTopology::Triangles)
        return;
    const bool skinned = prim.skin >= 0 && m_skinVbo && frame.joints && !frame.joints->empty() && frame.skinBase
                         && size_t(prim.skin) < frame.skinBase->size();

    QMatrix4x4 model;
    if (!skinned) {
        if (!frame.world || size_t(item.node) >= frame.world->size())
            return;
        model = (*frame.world)[size_t(item.node)];
        // Sphere against the frustum; the scale bound is the longest axis.
        const QVector3D c = model.map(item.center);
        const float scale = std::sqrt(std::max({model.column(0).toVector3D().lengthSquared(),
                                                model.column(1).toVector3D().lengthSquared(),
                                                model.column(2).toVector3D().lengthSquared()}));
        const float r = item.radius * scale;
        for (const QVector4D &plane : m_planes) {
            if (QVector3D::dotProduct(plane.toVector3D(), c) + plane.w() < -r)
                return;
        }
    }

    if (!wirePass) {
        const bool unlitTopology = prim.topology != ModelTopology::Triangles;
        const int key = (item.material + 1) * 2 + (unlitTopology ? 1 : 0);
        if (key != boundKey) {
            bindMaterial(item.material, unlitTopology);
            boundKey = key;
        }
        gl.glUniform1i(u.flat, prim.flatNormals);
    }
    gl.glUniform1i(u.skinned, skinned);
    if (skinned) {
        gl.glUniform1i(u.jointBase, (*frame.skinBase)[size_t(prim.skin)]);
        if (m_frontFaceCw) {
            gl.glFrontFace(GL_CCW);
            m_frontFaceCw = false;
        }
    } else {
        gl.glUniformMatrix4fv(u.model, 1, GL_FALSE, model.constData());
        const QMatrix3x3 normal = model.normalMatrix();
        gl.glUniformMatrix3fv(u.normalMatrix, 1, GL_FALSE, normal.constData());
        // Mirrored transforms flip the winding.
        const bool cw = model.determinant() < 0.0;
        if (cw != m_frontFaceCw) {
            gl.glFrontFace(cw ? GL_CW : GL_CCW);
            m_frontFaceCw = cw;
        }
    }

    const GLenum mode = prim.topology == ModelTopology::Lines    ? GL_LINES
                        : prim.topology == ModelTopology::Points ? GL_POINTS
                                                                 : GL_TRIANGLES;
    if (prim.indexCount > 0) {
        gl.glDrawElementsBaseVertex(mode, GLsizei(prim.indexCount), GL_UNSIGNED_INT,
                                    reinterpret_cast<void *>(prim.firstIndex * sizeof(quint32)),
                                    GLint(prim.baseVertex));
    } else {
        gl.glDrawArrays(mode, GLint(prim.baseVertex), GLsizei(prim.vertexCount));
    }
}

void ModelRenderer::render(const Frame &frame, const Settings &settings, const Colors &colors)
{
    QOpenGLFunctions_3_3_Core &gl = *m_gl;
    gl.glViewport(0, 0, frame.pixelSize.width(), frame.pixelSize.height());
    gl.glClearColor(colors.background.redF(), colors.background.greenF(), colors.background.blueF(), 1.0f);
    gl.glDepthMask(GL_TRUE);
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl.glEnable(GL_DEPTH_TEST);
    gl.glDepthFunc(GL_LEQUAL);
    gl.glDisable(GL_BLEND);
    gl.glDisable(GL_CULL_FACE);
    gl.glFrontFace(GL_CCW);
    m_cullOn = false;
    m_frontFaceCw = false;

    m_viewProj = frame.projection * frame.view;
    // Gribb-Hartmann frustum planes, normalised.
    const QVector4D r0 = m_viewProj.row(0), r1 = m_viewProj.row(1), r2 = m_viewProj.row(2), r3 = m_viewProj.row(3);
    m_planes = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2};
    for (QVector4D &p : m_planes) {
        const float len = p.toVector3D().length();
        if (len > 0.0f)
            p /= len;
    }

    const bool drawModel = m_doc && m_geometryDone && m_vao;
    const Programs &p = *m_programs;
    if (drawModel) {
        ensureJointBuffer(frame);
        m_programs->main.bind();
        const Uniforms &u = p.u;
        gl.glUniformMatrix4fv(u.viewProj, 1, GL_FALSE, m_viewProj.constData());
        gl.glUniform3f(u.eye, frame.eye.x(), frame.eye.y(), frame.eye.z());
        gl.glUniform1f(u.pointSize, std::max(1.0f, 2.0f * frame.devicePixelRatio));
        gl.glUniform1i(u.mode, int(settings.shading));

        // Two lights fixed to the camera (key upper-left, cool fill lower-right),
        // so the side the user looks at is always lit; the sky stays world-fixed.
        const QMatrix4x4 toWorld = frame.view.inverted();
        const QVector3D key = toWorld.mapVector(QVector3D(-0.45f, 0.7f, 0.55f)).normalized();
        const QVector3D fill = toWorld.mapVector(QVector3D(0.6f, -0.15f, 0.45f)).normalized();
        const GLfloat dirs[6] = {key.x(), key.y(), key.z(), fill.x(), fill.y(), fill.z()};
        const GLfloat lightColors[6] = {2.4f, 2.3f, 2.15f, 0.55f, 0.62f, 0.75f};
        gl.glUniform3fv(u.lightDir, 2, dirs);
        gl.glUniform3fv(u.lightColor, 2, lightColors);
        gl.glUniform3f(u.skyZenith, 0.62f, 0.66f, 0.74f);
        gl.glUniform3f(u.skyHorizon, 0.42f, 0.42f, 0.42f);
        gl.glUniform3f(u.skyGround, 0.12f, 0.11f, 0.10f);

        gl.glActiveTexture(GL_TEXTURE0 + JointUnit);
        gl.glBindTexture(GL_TEXTURE_BUFFER, m_jointCapacity > 0 ? m_jointTex : 0);
        gl.glActiveTexture(GL_TEXTURE0);
        gl.glBindVertexArray(m_vao);

        if (settings.wireframe) {
            gl.glEnable(GL_POLYGON_OFFSET_FILL);
            gl.glPolygonOffset(1.0f, 1.0f);
        }
        int bound = -1;
        for (const DrawItem &item : m_opaque)
            drawItem(item, frame, bound, false);
        gl.glBindVertexArray(0);
    }

    if (settings.grid)
        drawGrid(frame, colors);

    if (drawModel) {
        m_programs->main.bind();
        gl.glBindVertexArray(m_vao);
        if (!m_blended.empty()) {
            const QVector3D eye = frame.eye;
            const QVector3D forward = (frame.target - frame.eye).normalized();
            for (DrawItem &item : m_blended) {
                const QVector3D c = frame.world && size_t(item.node) < frame.world->size()
                                        ? (*frame.world)[size_t(item.node)].map(item.center)
                                        : item.center;
                item.depth = QVector3D::dotProduct(c - eye, forward);
            }
            std::sort(m_blended.begin(), m_blended.end(),
                      [](const DrawItem &a, const DrawItem &b) { return a.depth > b.depth; });
            gl.glEnable(GL_BLEND);
            gl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            gl.glDepthMask(GL_FALSE);
            int bound = -1;
            for (const DrawItem &item : m_blended)
                drawItem(item, frame, bound, false);
            gl.glDepthMask(GL_TRUE);
            gl.glDisable(GL_BLEND);
        }
        if (settings.wireframe) {
            gl.glDisable(GL_POLYGON_OFFSET_FILL);
            gl.glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            gl.glEnable(GL_BLEND);
            gl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            gl.glDepthMask(GL_FALSE);
            const Uniforms &u = p.u;
            gl.glUniform1i(u.mode, 4);
            gl.glUniform4f(u.wireColor, colors.wire.redF(), colors.wire.greenF(), colors.wire.blueF(),
                           colors.wire.alphaF());
            int bound = -1;
            for (const DrawItem &item : m_opaque)
                drawItem(item, frame, bound, true);
            for (const DrawItem &item : m_blended)
                drawItem(item, frame, bound, true);
            gl.glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            gl.glDepthMask(GL_TRUE);
            gl.glDisable(GL_BLEND);
        }
        gl.glBindVertexArray(0);
        m_programs->main.release();
        if (m_cullOn) {
            gl.glDisable(GL_CULL_FACE);
            m_cullOn = false;
        }
        if (settings.skeleton && !m_jointNodes.empty())
            drawSkeleton(frame, colors);
    }
}

void ModelRenderer::drawGrid(const Frame &frame, const Colors &colors)
{
    QOpenGLFunctions_3_3_Core &gl = *m_gl;
    const Programs &p = *m_programs;
    const float r = std::max(frame.sceneRadius, 1e-6f);
    const float cell = std::pow(10.0f, std::floor(std::log10(r * 2.0f))) / 10.0f;
    const float distance = (frame.eye - frame.target).length();
    const QMatrix4x4 inverse = m_viewProj.inverted();

    m_programs->grid.bind();
    gl.glUniformMatrix4fv(p.g.invViewProj, 1, GL_FALSE, inverse.constData());
    gl.glUniformMatrix4fv(p.g.viewProj, 1, GL_FALSE, m_viewProj.constData());
    gl.glUniform1f(p.g.groundY, frame.groundY);
    gl.glUniform1f(p.g.cell, cell);
    gl.glUniform3f(p.g.center, frame.target.x(), frame.target.y(), frame.target.z());
    gl.glUniform1f(p.g.fadeNear, std::max(r * 2.5f, distance * 0.6f));
    gl.glUniform1f(p.g.fadeFar, std::max(r * 8.0f, distance * 2.0f));
    auto color = [&gl](GLint loc, const QColor &c) { gl.glUniform4f(loc, c.redF(), c.greenF(), c.blueF(), c.alphaF()); };
    color(p.g.minor, colors.gridMinor);
    color(p.g.major, colors.gridMajor);
    color(p.g.xAxis, QColor(220, 80, 80, 200));
    color(p.g.zAxis, QColor(80, 120, 230, 200));

    gl.glEnable(GL_BLEND);
    gl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl.glDepthMask(GL_FALSE);
    gl.glBindVertexArray(m_emptyVao);
    gl.glDrawArrays(GL_TRIANGLES, 0, 3);
    gl.glBindVertexArray(0);
    gl.glDepthMask(GL_TRUE);
    gl.glDisable(GL_BLEND);
    m_programs->grid.release();
}

void ModelRenderer::drawSkeleton(const Frame &frame, const Colors &colors)
{
    if (!frame.world)
        return;
    QOpenGLFunctions_3_3_Core &gl = *m_gl;
    const Programs &p = *m_programs;
    const std::vector<QMatrix4x4> &world = *frame.world;
    auto position = [&world](int node, float *out) {
        const float *m = world[size_t(node)].constData(); // column-major: translation is column 3
        out[0] = m[12];
        out[1] = m[13];
        out[2] = m[14];
    };
    for (const int node : m_jointNodes) {
        if (size_t(node) >= world.size())
            return;
    }
    m_lineScratch.resize((m_bones.size() * 2 + m_jointNodes.size()) * 3);
    float *dst = m_lineScratch.data();
    for (const auto &[child, parent] : m_bones) {
        position(m_jointNodes[size_t(child)], dst);
        position(m_jointNodes[size_t(parent)], dst + 3);
        dst += 6;
    }
    for (const int node : m_jointNodes) {
        position(node, dst);
        dst += 3;
    }

    gl.glBindBuffer(GL_ARRAY_BUFFER, m_lineVbo);
    gl.glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(m_lineScratch.size() * sizeof(float)), m_lineScratch.data(),
                    GL_STREAM_DRAW);
    gl.glBindBuffer(GL_ARRAY_BUFFER, 0);

    m_programs->line.bind();
    gl.glUniformMatrix4fv(p.l.viewProj, 1, GL_FALSE, m_viewProj.constData());
    gl.glUniform4f(p.l.color, colors.skeleton.redF(), colors.skeleton.greenF(), colors.skeleton.blueF(),
                   colors.skeleton.alphaF());
    gl.glDisable(GL_DEPTH_TEST); // bones sit inside the mesh
    gl.glEnable(GL_BLEND);
    gl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl.glBindVertexArray(m_lineVao);
    gl.glUniform1i(p.l.round, 0);
    gl.glUniform1f(p.l.pointSize, 1.0f);
    gl.glDrawArrays(GL_LINES, 0, GLsizei(m_bones.size() * 2));
    gl.glUniform1i(p.l.round, 1);
    gl.glUniform1f(p.l.pointSize, 6.0f * frame.devicePixelRatio);
    gl.glDrawArrays(GL_POINTS, GLsizei(m_bones.size() * 2), GLsizei(m_jointNodes.size()));
    gl.glBindVertexArray(0);
    gl.glDisable(GL_BLEND);
    gl.glEnable(GL_DEPTH_TEST);
    m_programs->line.release();
}
