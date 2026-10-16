#include "GltfLoader.h"

#include <cgltf.h>

#include <QFile>
#include <QObject>
#include <QtConcurrent>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <unordered_map>

namespace GltfLoader {

namespace {

using ModelLoader::Result;

constexpr int kMaxInstances = 4096; // EXT_mesh_gpu_instancing copies expanded into nodes

// External buffer files, memory-mapped for cgltf and released through its
// release callback (cgltf_free). Loading is single-threaded.
struct BufferFiles {
    struct Held {
        std::unique_ptr<QFile> file;
        QByteArray copy;
    };
    std::unordered_map<const void *, Held> held;
};

cgltf_result readBufferFile(const cgltf_memory_options *, const cgltf_file_options *fileOptions, const char *path,
                            cgltf_size *size, void **data)
{
    auto *files = static_cast<BufferFiles *>(fileOptions->user_data);
    BufferFiles::Held h;
    h.file = std::make_unique<QFile>(QString::fromUtf8(path));
    if (!h.file->open(QIODevice::ReadOnly))
        return cgltf_result_file_not_found;
    const qint64 fileSize = h.file->size();
    const qint64 want = (size && *size) ? qint64(*size) : fileSize;
    if (want <= 0 || want > fileSize || want > ModelLoader::kMaxFileBytes)
        return cgltf_result_io_error;
    void *ptr = h.file->map(0, want);
    if (!ptr) {
        h.copy = h.file->read(want);
        if (h.copy.size() != want)
            return cgltf_result_io_error;
        ptr = h.copy.data();
        h.file.reset();
    }
    if (size)
        *size = cgltf_size(want);
    *data = ptr;
    files->held.emplace(ptr, std::move(h)); // QByteArray keeps its buffer when moved
    return cgltf_result_success;
}

void releaseBufferFile(const cgltf_memory_options *, const cgltf_file_options *fileOptions, void *data)
{
    static_cast<BufferFiles *>(fileOptions->user_data)->held.erase(data);
}

QString resultText(cgltf_result r)
{
    switch (r) {
    case cgltf_result_data_too_short: return QObject::tr("the file is truncated");
    case cgltf_result_unknown_format: return QObject::tr("not a glTF file");
    case cgltf_result_invalid_json: return QObject::tr("invalid JSON");
    case cgltf_result_invalid_gltf: return QObject::tr("invalid glTF");
    case cgltf_result_file_not_found: return QObject::tr("a referenced buffer file was not found");
    case cgltf_result_io_error: return QObject::tr("a referenced buffer could not be read");
    case cgltf_result_out_of_memory: return QObject::tr("out of memory");
    case cgltf_result_legacy_gltf: return QObject::tr("glTF 1.0 is not supported");
    default: return QObject::tr("error %1").arg(int(r));
    }
}

QMatrix4x4 fromColumnMajor(const float *m)
{
    return QMatrix4x4(m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14], m[3], m[7], m[11],
                      m[15]);
}

// Calls fn(i, v) for every element; v holds the element's components as
// floats (normalized integers converted). Tightly packed float data is read
// straight from the buffer.
template <typename Fn>
void forEachFloat(const cgltf_accessor *acc, Fn &&fn)
{
    const size_t count = acc->count;
    const size_t comps = std::min<size_t>(cgltf_num_components(acc->type), 16);
    float v[16] = {};
    if (acc->is_sparse || !acc->buffer_view || !cgltf_buffer_view_data(acc->buffer_view)) {
        std::vector<float> all(count * comps);
        cgltf_accessor_unpack_floats(acc, all.data(), all.size());
        for (size_t i = 0; i < count; ++i) {
            std::copy_n(all.data() + i * comps, comps, v);
            fn(i, static_cast<const float *>(v));
        }
        return;
    }
    if (acc->component_type == cgltf_component_type_r_32f) {
        const uint8_t *base = cgltf_buffer_view_data(acc->buffer_view) + acc->offset;
        const size_t stride = acc->stride;
        for (size_t i = 0; i < count; ++i) {
            std::memcpy(v, base + i * stride, comps * sizeof(float));
            fn(i, static_cast<const float *>(v));
        }
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        cgltf_accessor_read_float(acc, i, v, 16);
        fn(i, static_cast<const float *>(v));
    }
}

template <typename Fn>
void forEachUint4(const cgltf_accessor *acc, Fn &&fn)
{
    cgltf_uint v[4] = {0, 0, 0, 0};
    if (!acc->is_sparse && acc->buffer_view && cgltf_buffer_view_data(acc->buffer_view)) {
        const uint8_t *base = cgltf_buffer_view_data(acc->buffer_view) + acc->offset;
        const size_t stride = acc->stride;
        if (acc->component_type == cgltf_component_type_r_8u && acc->type == cgltf_type_vec4) {
            for (size_t i = 0; i < acc->count; ++i) {
                const uint8_t *p = base + i * stride;
                v[0] = p[0]; v[1] = p[1]; v[2] = p[2]; v[3] = p[3];
                fn(i, static_cast<const cgltf_uint *>(v));
            }
            return;
        }
        if (acc->component_type == cgltf_component_type_r_16u && acc->type == cgltf_type_vec4) {
            for (size_t i = 0; i < acc->count; ++i) {
                quint16 s[4];
                std::memcpy(s, base + i * stride, sizeof(s));
                v[0] = s[0]; v[1] = s[1]; v[2] = s[2]; v[3] = s[3];
                fn(i, static_cast<const cgltf_uint *>(v));
            }
            return;
        }
        for (size_t i = 0; i < acc->count; ++i) {
            cgltf_accessor_read_uint(acc, i, v, 4);
            fn(i, static_cast<const cgltf_uint *>(v));
        }
        return;
    }
    forEachFloat(acc, [&](size_t i, const float *f) {
        for (int c = 0; c < 4; ++c)
            v[c] = cgltf_uint(std::max(0.0f, f[c]));
        fn(i, static_cast<const cgltf_uint *>(v));
    });
}

enum class Conv : quint8 { None, List, Strip, Fan, LineStrip, LineLoop };

struct PrimJob {
    const cgltf_primitive *src = nullptr;
    size_t primitive = 0;          // into doc.primitives
    quint32 vertexOffset = 0;
    quint64 indexOffset = 0;
    quint32 vertexCount = 0;
    quint32 sourceIndexCount = 0;
    quint32 indexCount = 0;
    Conv conv = Conv::None;
    bool skinned = false;
};

ModelWrap wrapFor(cgltf_wrap_mode mode)
{
    switch (mode) {
    case cgltf_wrap_mode_clamp_to_edge: return ModelWrap::Clamp;
    case cgltf_wrap_mode_mirrored_repeat: return ModelWrap::Mirror;
    default: return ModelWrap::Repeat;
    }
}

class Converter
{
public:
    Converter(const QString &path, cgltf_data *data, const cgltf_options &options, ModelDocument &doc)
        : m_path(path), m_data(data), m_options(options), m_doc(doc), m_textures(path)
    {
    }

    ~Converter()
    {
        for (void *p : m_blobs)
            std::free(p);
    }

    // Empty on success; *tooLargeBytes is set when the geometry exceeds the cap.
    QString run(const std::atomic_bool &cancel, quint64 *tooLargeBytes);

private:
    int textureFor(const cgltf_texture_view &view, bool srgb);
    ModelMaterial material(const cgltf_material &m);
    void buildNodes();
    void buildSkins();
    void buildAnimations();
    void convert(PrimJob &job) const;

    QString m_path;
    cgltf_data *m_data;
    const cgltf_options &m_options;
    ModelDocument &m_doc;
    ModelLoader::TextureSet m_textures;
    std::vector<void *> m_blobs;        // decoded data-URI images, alive until decode
    std::vector<int> m_nodeMap;         // cgltf node -> doc node
    std::vector<int> m_meshSkin;        // cgltf mesh -> skin of the first node using it
    bool m_secondUvSkipped = false;
};

int Converter::textureFor(const cgltf_texture_view &view, bool srgb)
{
    const cgltf_texture *tex = view.texture;
    if (!tex)
        return -1;
    const int texcoord = view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord;
    if (texcoord != 0) {
        m_secondUvSkipped = true; // only TEXCOORD_0 is kept
        return -1;
    }
    const cgltf_image *image = tex->has_webp && tex->webp_image ? tex->webp_image : tex->image;
    const QString name = image && image->name ? QString::fromUtf8(image->name)
                         : image && image->uri && std::strncmp(image->uri, "data:", 5) != 0
                             ? QString::fromUtf8(image->uri)
                             : QObject::tr("image %1").arg(cgltf_texture_index(m_data, tex));
    if (!image)
        return tex->has_basisu ? m_textures.unsupported(m_doc, name, srgb) : -1;

    ModelWrap wrapS = ModelWrap::Repeat;
    ModelWrap wrapT = ModelWrap::Repeat;
    bool nearest = false;
    if (const cgltf_sampler *s = tex->sampler) {
        wrapS = wrapFor(s->wrap_s);
        wrapT = wrapFor(s->wrap_t);
        nearest = s->mag_filter == cgltf_filter_type_nearest;
    }
    const QString mime = image->mime_type ? QString::fromUtf8(image->mime_type) : QString();
    const QString key = QString::number(cgltf_image_index(m_data, image));

    if (image->buffer_view) {
        const uint8_t *bytes = cgltf_buffer_view_data(image->buffer_view);
        if (!bytes)
            return -1;
        return m_textures.fromBytes(m_doc, key,
                                    QByteArrayView(reinterpret_cast<const char *>(bytes), qsizetype(image->buffer_view->size)),
                                    mime, srgb, wrapS, wrapT, nearest, name);
    }
    if (!image->uri)
        return -1;
    if (std::strncmp(image->uri, "data:", 5) == 0) {
        const char *comma = std::strchr(image->uri, ',');
        if (!comma || comma - image->uri < 7 || std::strncmp(comma - 7, ";base64", 7) != 0)
            return -1;
        const char *b64 = comma + 1;
        const size_t len = std::strlen(b64);
        size_t padding = 0;
        if (len >= 1 && b64[len - 1] == '=') ++padding;
        if (len >= 2 && b64[len - 2] == '=') ++padding;
        const size_t decoded = len * 3 / 4 - std::min(padding, len * 3 / 4);
        void *out = nullptr;
        if (cgltf_load_buffer_base64(&m_options, decoded, b64, &out) != cgltf_result_success || !out)
            return -1;
        m_blobs.push_back(out);
        const QString dataMime = QString::fromLatin1(image->uri + 5, int(comma - 7 - (image->uri + 5)));
        return m_textures.fromBytes(m_doc, key, QByteArrayView(static_cast<const char *>(out), qsizetype(decoded)),
                                    mime.isEmpty() ? dataMime : mime, srgb, wrapS, wrapT, nearest,
                                    QObject::tr("embedded image"));
    }
    QByteArray uri(image->uri);
    uri.resize(qsizetype(cgltf_decode_uri(uri.data())));
    return m_textures.fromFile(m_doc, QString::fromUtf8(uri), srgb, wrapS, wrapT, nearest);
}

ModelMaterial Converter::material(const cgltf_material &m)
{
    ModelMaterial out;
    out.name = m.name ? QString::fromUtf8(m.name) : QString();
    const cgltf_texture_view *transformSource = nullptr;
    if (m.has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness &pbr = m.pbr_metallic_roughness;
        std::copy_n(pbr.base_color_factor, 4, out.baseColor.begin());
        out.metallic = pbr.metallic_factor;
        out.roughness = pbr.roughness_factor;
        out.baseColorTexture = textureFor(pbr.base_color_texture, true);
        const int mr = textureFor(pbr.metallic_roughness_texture, false);
        out.metallicTexture = mr;
        out.roughnessTexture = mr;
        out.metallicChannel = 2;
        out.roughnessChannel = 1;
        transformSource = &pbr.base_color_texture;
    } else if (m.has_pbr_specular_glossiness) {
        const cgltf_pbr_specular_glossiness &sg = m.pbr_specular_glossiness;
        std::copy_n(sg.diffuse_factor, 4, out.baseColor.begin());
        out.metallic = 0.0f;
        out.roughness = 1.0f - sg.glossiness_factor;
        out.baseColorTexture = textureFor(sg.diffuse_texture, true);
        transformSource = &sg.diffuse_texture;
    } else {
        // glTF's default material is white, fully metallic and rough, which
        // reads as black under an environment; keep the viewer's neutral look.
        out.metallic = 0.0f;
        out.roughness = 0.6f;
    }
    out.normalTexture = textureFor(m.normal_texture, false);
    out.normalScale = m.normal_texture.texture ? m.normal_texture.scale : 1.0f;
    out.occlusionTexture = textureFor(m.occlusion_texture, false);
    out.occlusionStrength = m.occlusion_texture.texture ? m.occlusion_texture.scale : 1.0f;
    out.emissiveTexture = textureFor(m.emissive_texture, true);
    const float strength = m.has_emissive_strength ? m.emissive_strength.emissive_strength : 1.0f;
    for (int i = 0; i < 3; ++i)
        out.emissive[size_t(i)] = m.emissive_factor[i] * strength;
    out.alphaMode = m.alpha_mode == cgltf_alpha_mode_mask    ? ModelAlphaMode::Mask
                    : m.alpha_mode == cgltf_alpha_mode_blend ? ModelAlphaMode::Blend
                                                             : ModelAlphaMode::Opaque;
    out.alphaCutoff = m.alpha_cutoff;
    out.doubleSided = m.double_sided;
    out.unlit = m.unlit;
    if (transformSource && transformSource->has_transform) {
        const cgltf_texture_transform &t = transformSource->transform;
        out.uvOffset = {t.offset[0], t.offset[1]};
        out.uvScale = {t.scale[0], t.scale[1]};
        out.uvRotation = t.rotation;
    }
    return out;
}

void Converter::buildNodes()
{
    m_nodeMap.assign(m_data->nodes_count, -1);
    std::vector<const cgltf_node *> roots;
    const cgltf_scene *scene = m_data->scene ? m_data->scene : (m_data->scenes_count ? &m_data->scenes[0] : nullptr);
    if (scene) {
        roots.assign(scene->nodes, scene->nodes + scene->nodes_count);
    } else {
        for (size_t i = 0; i < m_data->nodes_count; ++i)
            if (!m_data->nodes[i].parent)
                roots.push_back(&m_data->nodes[i]);
    }

    // Depth-first, parents before children; iterative so deep rigs cannot
    // overflow the stack, and a node is visited once even in broken files.
    std::vector<std::pair<const cgltf_node *, int>> stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
        stack.emplace_back(*it, -1);
    int instancesDropped = 0;
    while (!stack.empty()) {
        const auto [src, parent] = stack.back();
        stack.pop_back();
        const size_t srcIndex = cgltf_node_index(m_data, src);
        if (srcIndex >= m_nodeMap.size() || m_nodeMap[srcIndex] >= 0)
            continue;

        ModelNode node;
        node.name = src->name ? QString::fromUtf8(src->name) : QString();
        node.parent = parent;
        if (src->has_matrix) {
            node.hasMatrix = true;
            node.matrix = fromColumnMajor(src->matrix);
        } else {
            if (src->has_translation)
                node.translation = QVector3D(src->translation[0], src->translation[1], src->translation[2]);
            if (src->has_rotation)
                node.rotation = QQuaternion(src->rotation[3], src->rotation[0], src->rotation[1], src->rotation[2]);
            if (src->has_scale)
                node.scale = QVector3D(src->scale[0], src->scale[1], src->scale[2]);
        }
        const int meshIndex = src->mesh ? int(cgltf_mesh_index(m_data, src->mesh)) : -1;
        const int self = int(m_doc.nodes.size());
        m_nodeMap[srcIndex] = self;

        if (meshIndex >= 0 && src->has_mesh_gpu_instancing) {
            m_doc.nodes.push_back(std::move(node));
            // Instances become child nodes sharing the mesh.
            const cgltf_mesh_gpu_instancing &inst = src->mesh_gpu_instancing;
            const cgltf_accessor *t = nullptr, *r = nullptr, *s = nullptr;
            for (size_t a = 0; a < inst.attributes_count; ++a) {
                const char *n = inst.attributes[a].name;
                if (!n) continue;
                if (!std::strcmp(n, "TRANSLATION")) t = inst.attributes[a].data;
                else if (!std::strcmp(n, "ROTATION")) r = inst.attributes[a].data;
                else if (!std::strcmp(n, "SCALE")) s = inst.attributes[a].data;
            }
            const size_t count = t ? t->count : r ? r->count : s ? s->count : 0;
            const size_t kept = std::min<size_t>(count, kMaxInstances);
            instancesDropped += int(count - kept);
            const size_t first = m_doc.nodes.size();
            m_doc.nodes.resize(first + kept);
            for (size_t k = 0; k < kept; ++k) {
                m_doc.nodes[first + k].parent = self;
                m_doc.nodes[first + k].mesh = meshIndex;
            }
            if (t) forEachFloat(t, [&](size_t i, const float *v) {
                if (i < kept) m_doc.nodes[first + i].translation = QVector3D(v[0], v[1], v[2]);
            });
            if (r) forEachFloat(r, [&](size_t i, const float *v) {
                if (i < kept) m_doc.nodes[first + i].rotation = QQuaternion(v[3], v[0], v[1], v[2]).normalized();
            });
            if (s) forEachFloat(s, [&](size_t i, const float *v) {
                if (i < kept) m_doc.nodes[first + i].scale = QVector3D(v[0], v[1], v[2]);
            });
        } else {
            node.mesh = meshIndex;
            m_doc.nodes.push_back(std::move(node));
        }
        if (meshIndex >= 0 && src->skin && m_meshSkin[size_t(meshIndex)] < 0)
            m_meshSkin[size_t(meshIndex)] = int(cgltf_skin_index(m_data, src->skin));
        for (size_t c = src->children_count; c-- > 0;)
            stack.emplace_back(src->children[c], self);
    }
    if (instancesDropped > 0)
        m_doc.warnings << QObject::tr("%n instance(s) beyond %1 per node not shown", nullptr, instancesDropped)
                              .arg(kMaxInstances);
}

void Converter::buildSkins()
{
    m_doc.skins.resize(m_data->skins_count);
    for (size_t s = 0; s < m_data->skins_count; ++s) {
        const cgltf_skin &src = m_data->skins[s];
        ModelSkin &skin = m_doc.skins[s];
        skin.name = src.name ? QString::fromUtf8(src.name) : QString();
        skin.joints.resize(src.joints_count, -1);
        skin.inverseBind.resize(src.joints_count);
        for (size_t j = 0; j < src.joints_count; ++j) {
            const size_t n = cgltf_node_index(m_data, src.joints[j]);
            if (n < m_nodeMap.size())
                skin.joints[j] = m_nodeMap[n];
        }
        if (src.inverse_bind_matrices && src.inverse_bind_matrices->type == cgltf_type_mat4) {
            forEachFloat(src.inverse_bind_matrices, [&](size_t i, const float *v) {
                if (i < skin.inverseBind.size())
                    skin.inverseBind[i] = fromColumnMajor(v);
            });
        }
    }
}

void Converter::buildAnimations()
{
    int morphChannels = 0;
    for (size_t a = 0; a < m_data->animations_count; ++a) {
        const cgltf_animation &src = m_data->animations[a];
        ModelAnimation anim;
        anim.name = src.name && *src.name ? QString::fromUtf8(src.name) : QObject::tr("Animation %1").arg(a + 1);
        for (size_t c = 0; c < src.channels_count; ++c) {
            const cgltf_animation_channel &ch = src.channels[c];
            if (ch.target_path == cgltf_animation_path_type_weights) {
                ++morphChannels;
                continue;
            }
            if (!ch.target_node || !ch.sampler || !ch.sampler->input || !ch.sampler->output)
                continue;
            const size_t n = cgltf_node_index(m_data, ch.target_node);
            if (n >= m_nodeMap.size() || m_nodeMap[n] < 0)
                continue;
            ModelChannel out;
            out.node = m_nodeMap[n];
            switch (ch.target_path) {
            case cgltf_animation_path_type_translation: out.path = ModelChannelPath::Translation; break;
            case cgltf_animation_path_type_rotation: out.path = ModelChannelPath::Rotation; break;
            case cgltf_animation_path_type_scale: out.path = ModelChannelPath::Scale; break;
            default: continue;
            }
            switch (ch.sampler->interpolation) {
            case cgltf_interpolation_type_step: out.interpolation = ModelInterpolation::Step; break;
            case cgltf_interpolation_type_cubic_spline: out.interpolation = ModelInterpolation::CubicSpline; break;
            default: out.interpolation = ModelInterpolation::Linear; break;
            }
            const size_t keys = ch.sampler->input->count;
            const size_t comps = out.path == ModelChannelPath::Rotation ? 4 : 3;
            const size_t perKey = out.interpolation == ModelInterpolation::CubicSpline ? 3 : 1;
            if (keys == 0 || ch.sampler->output->count < keys * perKey)
                continue;
            out.times.resize(keys);
            forEachFloat(ch.sampler->input, [&](size_t i, const float *v) { out.times[i] = v[0]; });
            out.values.resize(keys * perKey * comps);
            forEachFloat(ch.sampler->output, [&](size_t i, const float *v) {
                if (i < keys * perKey)
                    std::copy_n(v, comps, out.values.begin() + qsizetype(i * comps));
            });
            anim.duration = std::max(anim.duration, out.times.back());
            anim.channels.push_back(std::move(out));
        }
        if (!anim.channels.empty())
            m_doc.animations.push_back(std::move(anim));
    }
    if (morphChannels > 0)
        m_doc.warnings << QObject::tr("morph target animation is not shown");
}

void Converter::convert(PrimJob &job) const
{
    const cgltf_primitive &p = *job.src;
    ModelVertex *out = m_doc.vertices.data() + job.vertexOffset;
    const quint32 n = job.vertexCount;

    for (size_t a = 0; a < p.attributes_count; ++a) {
        const cgltf_attribute &attr = p.attributes[a];
        const cgltf_accessor *acc = attr.data;
        if (!acc || acc->count < n)
            continue;
        switch (attr.type) {
        case cgltf_attribute_type_position:
            forEachFloat(acc, [&](size_t i, const float *v) {
                if (i < n) { out[i].px = v[0]; out[i].py = v[1]; out[i].pz = v[2]; }
            });
            break;
        case cgltf_attribute_type_normal:
            forEachFloat(acc, [&](size_t i, const float *v) {
                if (i < n) out[i].normal = ModelPack::normal(v[0], v[1], v[2]);
            });
            break;
        case cgltf_attribute_type_texcoord:
            if (attr.index == 0)
                forEachFloat(acc, [&](size_t i, const float *v) {
                    if (i < n) { out[i].u = v[0]; out[i].v = v[1]; }
                });
            break;
        case cgltf_attribute_type_color:
            if (attr.index == 0) {
                const bool rgba = acc->type == cgltf_type_vec4;
                forEachFloat(acc, [&](size_t i, const float *v) {
                    if (i < n) out[i].color = ModelPack::colorFromLinear(v[0], v[1], v[2], rgba ? v[3] : 1.0f);
                });
            }
            break;
        default:
            break;
        }
    }

    if (job.skinned) {
        const cgltf_accessor *joints = cgltf_find_accessor(&p, cgltf_attribute_type_joints, 0);
        const cgltf_accessor *weights = cgltf_find_accessor(&p, cgltf_attribute_type_weights, 0);
        ModelSkinVertex *skin = m_doc.skin.data() + job.vertexOffset;
        std::vector<quint32> j(size_t(n) * 4, 0);
        forEachUint4(joints, [&](size_t i, const cgltf_uint *v) {
            if (i < n) std::copy_n(v, 4, j.begin() + qsizetype(i * 4));
        });
        forEachFloat(weights, [&](size_t i, const float *v) {
            if (i < n) skin[i] = ModelPack::skinVertex(j.data() + i * 4, v, 4);
        });
    }

    if (job.indexCount == 0)
        return;
    quint32 *idx = m_doc.indices.data() + job.indexOffset;
    std::vector<quint32> src;
    const quint32 *s = nullptr;
    if (job.conv == Conv::List && p.indices) {
        cgltf_accessor_unpack_indices(p.indices, idx, sizeof(quint32), job.indexCount);
    } else {
        src.resize(job.sourceIndexCount);
        if (p.indices)
            cgltf_accessor_unpack_indices(p.indices, src.data(), sizeof(quint32), src.size());
        else
            std::iota(src.begin(), src.end(), 0u);
        s = src.data();
        const quint32 m = job.sourceIndexCount;
        quint32 *o = idx;
        switch (job.conv) {
        case Conv::Strip:
            for (quint32 k = 0; k + 2 < m; ++k) {
                const bool odd = k & 1;
                *o++ = s[odd ? k + 1 : k];
                *o++ = s[odd ? k : k + 1];
                *o++ = s[k + 2];
            }
            break;
        case Conv::Fan:
            for (quint32 k = 1; k + 1 < m; ++k) {
                *o++ = s[k];
                *o++ = s[k + 1];
                *o++ = s[0];
            }
            break;
        case Conv::LineStrip:
            for (quint32 k = 0; k + 1 < m; ++k) {
                *o++ = s[k];
                *o++ = s[k + 1];
            }
            break;
        case Conv::LineLoop:
            for (quint32 k = 0; k < m; ++k) {
                *o++ = s[k];
                *o++ = s[(k + 1) % m];
            }
            break;
        case Conv::List:
            std::copy_n(s, job.indexCount, o);
            break;
        case Conv::None:
            break;
        }
    }
    // Out-of-range indices would read past the primitive on the GPU.
    for (quint32 k = 0; k < job.indexCount; ++k)
        if (idx[k] >= n)
            idx[k] = 0;
}

QString Converter::run(const std::atomic_bool &cancel, quint64 *tooLargeBytes)
{
    for (size_t i = 0; i < m_data->extensions_required_count; ++i) {
        const char *ext = m_data->extensions_required[i];
        if (!std::strcmp(ext, "KHR_draco_mesh_compression"))
            return QObject::tr("This glTF uses Draco mesh compression, which the preview cannot decode.");
        if (!std::strcmp(ext, "EXT_meshopt_compression") || !std::strcmp(ext, "KHR_meshopt_compression"))
            return QObject::tr("This glTF uses meshopt compression, which the preview cannot decode.");
    }

    m_doc.format = m_data->file_type == cgltf_file_type_glb ? QObject::tr("glTF 2.0 binary") : QObject::tr("glTF 2.0");
    m_doc.cullBackFaces = true;
    if (m_data->asset.generator)
        m_doc.detail = QString::fromUtf8(m_data->asset.generator);

    m_meshSkin.assign(m_data->meshes_count, -1);
    buildNodes();
    buildSkins();

    // Materials (textures are only registered here, decoded below).
    m_doc.materials.reserve(m_data->materials_count);
    for (size_t i = 0; i < m_data->materials_count; ++i)
        m_doc.materials.push_back(material(m_data->materials[i]));

    // Sizes first, so the cap is checked before anything large is allocated.
    std::vector<PrimJob> jobs;
    quint64 vertices = 0;
    quint64 indices = 0;
    bool anySkin = false;
    bool morph = false;
    bool draco = false;
    m_doc.meshes.resize(m_data->meshes_count);
    for (size_t mi = 0; mi < m_data->meshes_count; ++mi) {
        const cgltf_mesh &mesh = m_data->meshes[mi];
        ModelMesh &out = m_doc.meshes[mi];
        out.name = mesh.name ? QString::fromUtf8(mesh.name) : QString();
        out.firstPrimitive = int(m_doc.primitives.size());
        for (size_t pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive &p = mesh.primitives[pi];
            const cgltf_accessor *pos = cgltf_find_accessor(&p, cgltf_attribute_type_position, 0);
            if (!pos || pos->count == 0)
                continue;
            if (p.has_draco_mesh_compression && !pos->buffer_view && !pos->is_sparse) {
                draco = true;
                continue;
            }
            morph = morph || p.targets_count > 0;
            PrimJob job;
            job.src = &p;
            job.vertexCount = quint32(std::min<size_t>(pos->count, 0x7FFFFFFF));
            job.sourceIndexCount = quint32(p.indices ? std::min<size_t>(p.indices->count, 0xFFFFFFFF) : job.vertexCount);
            ModelPrimitive prim;
            const quint32 m = job.sourceIndexCount;
            switch (p.type) {
            case cgltf_primitive_type_points:
                prim.topology = ModelTopology::Points;
                job.conv = p.indices ? Conv::List : Conv::None;
                job.indexCount = p.indices ? m : 0;
                break;
            case cgltf_primitive_type_lines:
                prim.topology = ModelTopology::Lines;
                job.conv = p.indices ? Conv::List : Conv::None;
                job.indexCount = p.indices ? m / 2 * 2 : 0;
                break;
            case cgltf_primitive_type_line_strip:
                prim.topology = ModelTopology::Lines;
                job.conv = Conv::LineStrip;
                job.indexCount = m >= 2 ? (m - 1) * 2 : 0;
                break;
            case cgltf_primitive_type_line_loop:
                prim.topology = ModelTopology::Lines;
                job.conv = Conv::LineLoop;
                job.indexCount = m >= 2 ? m * 2 : 0;
                break;
            case cgltf_primitive_type_triangle_strip:
                job.conv = Conv::Strip;
                job.indexCount = m >= 3 ? (m - 2) * 3 : 0;
                break;
            case cgltf_primitive_type_triangle_fan:
                job.conv = Conv::Fan;
                job.indexCount = m >= 3 ? (m - 2) * 3 : 0;
                break;
            default:
                job.conv = p.indices ? Conv::List : Conv::None;
                job.indexCount = p.indices ? m / 3 * 3 : 0;
                break;
            }
            if (job.conv != Conv::None && job.indexCount == 0)
                continue; // degenerate strip/fan/loop
            job.skinned = m_meshSkin[mi] >= 0 && cgltf_find_accessor(&p, cgltf_attribute_type_joints, 0)
                          && cgltf_find_accessor(&p, cgltf_attribute_type_weights, 0);
            anySkin = anySkin || job.skinned;

            prim.baseVertex = quint32(vertices);
            prim.vertexCount = job.vertexCount;
            prim.firstIndex = indices;
            prim.indexCount = job.indexCount;
            prim.material = p.material ? int(cgltf_material_index(m_data, p.material)) : -1;
            prim.skin = job.skinned ? m_meshSkin[mi] : -1;
            prim.flatNormals = prim.topology == ModelTopology::Triangles
                               && !cgltf_find_accessor(&p, cgltf_attribute_type_normal, 0);
            prim.hasColor = cgltf_find_accessor(&p, cgltf_attribute_type_color, 0) != nullptr;

            job.vertexOffset = quint32(vertices);
            job.indexOffset = indices;
            job.primitive = m_doc.primitives.size();
            vertices += job.vertexCount;
            indices += job.indexCount;
            m_doc.primitives.push_back(prim);
            jobs.push_back(job);
        }
        out.primitiveCount = int(m_doc.primitives.size()) - out.firstPrimitive;
    }
    if (draco && jobs.empty())
        return QObject::tr("This glTF uses Draco mesh compression, which the preview cannot decode.");
    if (draco)
        m_doc.warnings << QObject::tr("Draco-compressed primitives are not shown");
    if (morph)
        m_doc.warnings << QObject::tr("morph targets are not shown");

    const quint64 bytes = vertices * sizeof(ModelVertex) + (anySkin ? vertices * sizeof(ModelSkinVertex) : 0)
                          + indices * sizeof(quint32);
    if (bytes > quint64(ModelLoader::kMaxGeometryBytes)) {
        *tooLargeBytes = bytes;
        return QString();
    }
    if (cancel.load(std::memory_order_relaxed))
        return QString();

    m_doc.vertices.resize(size_t(vertices));
    if (anySkin)
        m_doc.skin.resize(size_t(vertices));
    m_doc.indices.resize(size_t(indices));

    if (jobs.size() > 1)
        QtConcurrent::blockingMap(jobs, [this](PrimJob &job) { convert(job); });
    else
        for (PrimJob &job : jobs)
            convert(job);
    if (cancel.load(std::memory_order_relaxed))
        return QString();

    buildAnimations();
    if (m_secondUvSkipped)
        m_doc.warnings << QObject::tr("maps on a second UV set are not shown");

    // Embedded images point into the mapped file / buffers: decode while
    // they are alive.
    m_textures.decode(m_doc, cancel);
    return QString();
}

} // namespace

Result load(const QString &path, QByteArrayView bytes, const std::atomic_bool &cancel)
{
    BufferFiles files; // outlives `data`: cgltf_free releases the mappings
    cgltf_options options{};
    options.file.read = readBufferFile;
    options.file.release = releaseBufferFile;
    options.file.user_data = &files;

    cgltf_data *raw = nullptr;
    cgltf_result res = cgltf_parse(&options, bytes.data(), cgltf_size(bytes.size()), &raw);
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, &cgltf_free);
    if (res != cgltf_result_success)
        return ModelLoader::failed(QObject::tr("Could not read the glTF: %1.").arg(resultText(res)));

    const QByteArray pathUtf8 = path.toUtf8();
    res = cgltf_load_buffers(&options, data.get(), pathUtf8.constData());
    if (res != cgltf_result_success)
        return ModelLoader::failed(QObject::tr("Could not read the glTF: %1.").arg(resultText(res)));
    for (size_t i = 0; i < data->buffers_count; ++i) {
        if (!data->buffers[i].data && data->buffers[i].size > 0)
            return ModelLoader::failed(QObject::tr("Could not read the glTF: buffer %1 could not be loaded.").arg(i));
    }
    // Bounds of every accessor against its buffer view, so the reads below
    // cannot run past the data.
    res = cgltf_validate(data.get());
    if (res != cgltf_result_success)
        return ModelLoader::failed(QObject::tr("Could not read the glTF: %1.").arg(resultText(res)));

    auto doc = std::make_shared<ModelDocument>();
    QString error;
    quint64 tooLargeBytes = 0;
    {
        Converter converter(path, data.get(), options, *doc);
        error = converter.run(cancel, &tooLargeBytes);
    }
    if (cancel.load(std::memory_order_relaxed))
        return ModelLoader::cancelled();
    if (tooLargeBytes)
        return ModelLoader::tooLarge(qint64(tooLargeBytes));
    if (!error.isEmpty())
        return ModelLoader::failed(error);
    ModelLoader::Result r;
    r.doc = std::move(doc);
    return r;
}

} // namespace GltfLoader
