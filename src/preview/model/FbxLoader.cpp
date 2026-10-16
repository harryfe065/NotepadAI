#include "FbxLoader.h"

#include <ufbx.h>

#include <QObject>
#include <QtConcurrent>

#include <algorithm>
#include <cstring>
#include <memory>

namespace FbxLoader {

namespace {

using ModelLoader::Result;

QString str(const ufbx_string &s)
{
    return QString::fromUtf8(s.data, qsizetype(s.length));
}

QMatrix4x4 toMatrix(const ufbx_matrix &m)
{
    return QMatrix4x4(float(m.m00), float(m.m01), float(m.m02), float(m.m03), float(m.m10), float(m.m11),
                      float(m.m12), float(m.m13), float(m.m20), float(m.m21), float(m.m22), float(m.m23), 0, 0, 0, 1);
}

ufbx_progress_result onProgress(void *user, const ufbx_progress *)
{
    return static_cast<const std::atomic_bool *>(user)->load(std::memory_order_relaxed) ? UFBX_PROGRESS_CANCEL
                                                                                       : UFBX_PROGRESS_CONTINUE;
}

inline quint8 unorm8(double v)
{
    if (!(v > 0.0)) return 0;
    if (v >= 1.0) return 255;
    return quint8(v * 255.0 + 0.5);
}

// One converted ufbx_mesh: local arrays, appended to the document afterwards.
struct MeshJob {
    const ufbx_mesh *mesh = nullptr;
    int skin = -1;                        // doc skin index of mesh->skin_deformers[0]
    std::vector<int> partMaterial;        // per material part, doc material index
    std::vector<ModelVertex> vertices;
    std::vector<ModelSkinVertex> skinVertices;
    std::vector<quint32> indices;
    struct Part {
        quint32 firstIndex = 0;
        quint32 indexCount = 0;
        quint32 baseVertex = 0;
        quint32 vertexCount = 0;
        int material = -1;
    };
    std::vector<Part> parts;
    QString error;
};

void convertMesh(MeshJob &job, const std::atomic_bool &cancel)
{
    const ufbx_mesh &mesh = *job.mesh;
    const ufbx_skin_deformer *skin = job.skin >= 0 && mesh.skin_deformers.count ? mesh.skin_deformers.data[0] : nullptr;
    const bool hasUv = mesh.vertex_uv.exists;
    const bool hasColor = mesh.vertex_color.exists;
    const bool hasNormal = mesh.vertex_normal.exists;

    std::vector<uint32_t> tri(mesh.max_face_triangles * 3);
    std::vector<ModelVertex> soup;
    std::vector<ModelSkinVertex> soupSkin;
    std::vector<uint32_t> partIndices;

    // A mesh without material parts is one part over every face.
    const size_t partCount = mesh.material_parts.count ? mesh.material_parts.count : 1;
    for (size_t pi = 0; pi < partCount; ++pi) {
        if (cancel.load(std::memory_order_relaxed))
            return;
        const ufbx_mesh_part *part = mesh.material_parts.count ? &mesh.material_parts.data[pi] : nullptr;
        const size_t faceCount = part ? part->num_faces : mesh.num_faces;
        const size_t triCount = part ? part->num_triangles : mesh.num_triangles;
        if (triCount == 0)
            continue;
        soup.clear();
        soup.reserve(triCount * 3);
        soupSkin.clear();
        if (skin)
            soupSkin.reserve(triCount * 3);

        for (size_t f = 0; f < faceCount; ++f) {
            const uint32_t faceIndex = part ? part->face_indices.data[f] : uint32_t(f);
            const ufbx_face face = mesh.faces.data[faceIndex];
            const uint32_t n = ufbx_triangulate_face(tri.data(), tri.size(), &mesh, face);
            for (uint32_t c = 0; c < n * 3; ++c) {
                const uint32_t ix = tri[c];
                ModelVertex v;
                const ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh.vertex_position, ix);
                v.px = float(p.x);
                v.py = float(p.y);
                v.pz = float(p.z);
                if (hasNormal) {
                    const ufbx_vec3 nn = ufbx_get_vertex_vec3(&mesh.vertex_normal, ix);
                    v.normal = ModelPack::normal(float(nn.x), float(nn.y), float(nn.z));
                }
                if (hasUv) {
                    const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, ix);
                    v.u = float(uv.x);
                    v.v = 1.0f - float(uv.y); // FBX UVs are bottom-up
                }
                if (hasColor) {
                    // FBX vertex colours are stored as authored (display values).
                    const ufbx_vec4 col = ufbx_get_vertex_vec4(&mesh.vertex_color, ix);
                    v.color = ModelPack::colorFromBytes(unorm8(col.x), unorm8(col.y), unorm8(col.z), unorm8(col.w));
                }
                soup.push_back(v);
                if (skin) {
                    const uint32_t vertex = mesh.vertex_indices.data[ix];
                    ModelSkinVertex sv;
                    if (vertex < skin->vertices.count) {
                        const ufbx_skin_vertex &w = skin->vertices.data[vertex];
                        quint32 joints[8];
                        float weights[8];
                        const int count = int(std::min<uint32_t>(w.num_weights, 8)); // sorted by weight
                        for (int k = 0; k < count; ++k) {
                            const ufbx_skin_weight &sw = skin->weights.data[w.weight_begin + uint32_t(k)];
                            joints[k] = sw.cluster_index;
                            weights[k] = float(sw.weight);
                        }
                        sv = ModelPack::skinVertex(joints, weights, count);
                    } else {
                        sv.weights[0] = 255;
                    }
                    soupSkin.push_back(sv);
                }
            }
        }

        // Weld identical corners: compacts the streams in place.
        partIndices.resize(soup.size());
        ufbx_vertex_stream streams[2] = {
            {soup.data(), soup.size(), sizeof(ModelVertex)},
            {soupSkin.data(), soupSkin.size(), sizeof(ModelSkinVertex)},
        };
        ufbx_error error;
        const size_t unique = ufbx_generate_indices(streams, skin ? 2 : 1, partIndices.data(), partIndices.size(),
                                                    nullptr, &error);
        if (error.type != UFBX_ERROR_NONE) {
            job.error = str(error.description);
            return;
        }

        MeshJob::Part out;
        out.firstIndex = quint32(job.indices.size());
        out.indexCount = quint32(partIndices.size());
        out.baseVertex = quint32(job.vertices.size());
        out.vertexCount = quint32(unique);
        out.material = pi < job.partMaterial.size() ? job.partMaterial[pi] : -1;
        job.parts.push_back(out);
        job.vertices.insert(job.vertices.end(), soup.begin(), soup.begin() + qsizetype(unique));
        if (skin)
            job.skinVertices.insert(job.skinVertices.end(), soupSkin.begin(), soupSkin.begin() + qsizetype(unique));
        job.indices.insert(job.indices.end(), partIndices.begin(), partIndices.end());
    }
    job.vertices.shrink_to_fit();
    job.skinVertices.shrink_to_fit();
    job.indices.shrink_to_fit();
}

class Converter
{
public:
    Converter(const QString &path, const ufbx_scene &scene, ModelDocument &doc)
        : m_scene(scene), m_doc(doc), m_textures(path)
    {
    }

    QString run(const std::atomic_bool &cancel, quint64 *tooLargeBytes);

private:
    int texture(const ufbx_texture *tex, bool srgb);
    ModelMaterial material(const ufbx_material &m);
    void buildAnimations(const std::atomic_bool &cancel);

    const ufbx_scene &m_scene;
    ModelDocument &m_doc;
    ModelLoader::TextureSet m_textures;
    std::vector<int> m_nodeMap;   // ufbx node typed_id -> doc node
};

int Converter::texture(const ufbx_texture *tex, bool srgb)
{
    if (!tex)
        return -1;
    if (tex->type != UFBX_TEXTURE_FILE && tex->file_textures.count)
        tex = tex->file_textures.data[0]; // layered / shader: first file layer
    const ModelWrap wrapS = tex->wrap_u == UFBX_WRAP_CLAMP ? ModelWrap::Clamp : ModelWrap::Repeat;
    const ModelWrap wrapT = tex->wrap_v == UFBX_WRAP_CLAMP ? ModelWrap::Clamp : ModelWrap::Repeat;
    const QString file = str(tex->filename).isEmpty() ? str(tex->relative_filename) : str(tex->filename);
    if (tex->content.size > 0) {
        const QString key = QString::number(quintptr(tex->content.data), 16);
        const QString name = file.isEmpty() ? str(tex->name) : file.section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1);
        return m_textures.fromBytes(m_doc, key,
                                    QByteArrayView(static_cast<const char *>(tex->content.data), qsizetype(tex->content.size)),
                                    file.section(QLatin1Char('.'), -1), srgb, wrapS, wrapT, false, name);
    }
    if (!file.isEmpty())
        return m_textures.fromFile(m_doc, file, srgb, wrapS, wrapT, false);
    if (tex->absolute_filename.length)
        return m_textures.fromFile(m_doc, str(tex->absolute_filename), srgb, wrapS, wrapT, false);
    return -1;
}

ModelMaterial Converter::material(const ufbx_material &m)
{
    ModelMaterial out;
    out.name = str(m.name);
    const ufbx_material_pbr_maps &p = m.pbr;
    const double factor = p.base_factor.has_value ? p.base_factor.value_real : 1.0;
    for (int i = 0; i < 3; ++i)
        out.baseColor[size_t(i)] = ModelPack::srgbToLinear(float(p.base_color.value_vec4.v[i])) * float(factor);
    out.baseColor[3] = p.opacity.has_value ? float(p.opacity.value_real) : 1.0f;
    out.roughness = p.roughness.has_value ? float(p.roughness.value_real) : 0.6f;
    out.metallic = p.metalness.has_value ? float(p.metalness.value_real) : 0.0f;
    const double emission = p.emission_factor.has_value ? p.emission_factor.value_real : 1.0;
    for (int i = 0; i < 3; ++i)
        out.emissive[size_t(i)] = ModelPack::srgbToLinear(float(p.emission_color.value_vec3.v[i])) * float(emission);

    out.baseColorTexture = p.base_color.texture_enabled || p.base_color.texture ? texture(p.base_color.texture, true) : -1;
    out.roughnessTexture = texture(p.roughness.texture, false);
    out.roughnessChannel = 0;
    out.metallicTexture = texture(p.metalness.texture, false);
    out.metallicChannel = 0;
    out.normalTexture = texture(p.normal_map.texture, false);
    out.occlusionTexture = texture(p.ambient_occlusion.texture, false);
    out.emissiveTexture = texture(p.emission_color.texture, true);
    if (out.emissiveTexture >= 0 && out.emissive == std::array<float, 3>{0, 0, 0})
        out.emissive = {1, 1, 1};

    if (p.opacity.texture)
        out.alphaMode = ModelAlphaMode::Mask; // cut-outs (foliage, hair cards)
    else if (out.baseColor[3] < 0.999f)
        out.alphaMode = ModelAlphaMode::Blend;
    out.doubleSided = m.features.double_sided.enabled;
    out.unlit = m.features.unlit.enabled;

    if (const ufbx_texture *t = p.base_color.texture; t && t->has_uv_transform) {
        const float sx = float(t->uv_transform.scale.x);
        const float sy = float(t->uv_transform.scale.y);
        if (sx != 0.0f && sy != 0.0f) {
            out.uvScale = {sx, sy};
            out.uvOffset = {0.0f, 1.0f - sy}; // tiling about the bottom edge, V flipped
        }
    }
    return out;
}

void Converter::buildAnimations(const std::atomic_bool &cancel)
{
    for (size_t s = 0; s < m_scene.anim_stacks.count; ++s) {
        if (cancel.load(std::memory_order_relaxed))
            return;
        const ufbx_anim_stack &stack = *m_scene.anim_stacks.data[s];
        ufbx_bake_opts opts = {};
        opts.trim_start_time = true;
        opts.resample_rate = 30.0;
        opts.key_reduction_enabled = true;
        opts.key_reduction_rotation = true;
        ufbx_error error;
        std::unique_ptr<ufbx_baked_anim, decltype(&ufbx_free_baked_anim)> baked(
            ufbx_bake_anim(&m_scene, stack.anim, &opts, &error), &ufbx_free_baked_anim);
        if (!baked)
            continue;

        ModelAnimation anim;
        anim.name = stack.name.length ? str(stack.name) : QObject::tr("Take %1").arg(s + 1);
        anim.duration = float(std::max(baked->playback_duration, baked->key_time_max));
        for (size_t n = 0; n < baked->nodes.count; ++n) {
            const ufbx_baked_node &bn = baked->nodes.data[n];
            if (bn.typed_id >= m_nodeMap.size() || m_nodeMap[bn.typed_id] < 0)
                continue;
            const int node = m_nodeMap[bn.typed_id];
            auto addVec3 = [&](const ufbx_baked_vec3_list &keys, ModelChannelPath path) {
                if (!keys.count)
                    return;
                ModelChannel ch;
                ch.node = node;
                ch.path = path;
                ch.interpolation = ModelInterpolation::Linear;
                ch.times.reserve(keys.count);
                ch.values.reserve(keys.count * 3);
                for (size_t k = 0; k < keys.count; ++k) {
                    ch.times.push_back(float(keys.data[k].time));
                    ch.values.push_back(float(keys.data[k].value.x));
                    ch.values.push_back(float(keys.data[k].value.y));
                    ch.values.push_back(float(keys.data[k].value.z));
                }
                anim.channels.push_back(std::move(ch));
            };
            addVec3(bn.translation_keys, ModelChannelPath::Translation);
            addVec3(bn.scale_keys, ModelChannelPath::Scale);
            if (bn.rotation_keys.count) {
                ModelChannel ch;
                ch.node = node;
                ch.path = ModelChannelPath::Rotation;
                ch.interpolation = ModelInterpolation::Linear;
                ch.times.reserve(bn.rotation_keys.count);
                ch.values.reserve(bn.rotation_keys.count * 4);
                for (size_t k = 0; k < bn.rotation_keys.count; ++k) {
                    const ufbx_baked_quat &q = bn.rotation_keys.data[k];
                    ch.times.push_back(float(q.time));
                    ch.values.push_back(float(q.value.x));
                    ch.values.push_back(float(q.value.y));
                    ch.values.push_back(float(q.value.z));
                    ch.values.push_back(float(q.value.w));
                }
                anim.channels.push_back(std::move(ch));
            }
        }
        if (!anim.channels.empty())
            m_doc.animations.push_back(std::move(anim));
    }
}

QString Converter::run(const std::atomic_bool &cancel, quint64 *tooLargeBytes)
{
    const ufbx_metadata &meta = m_scene.metadata;
    m_doc.format = QObject::tr("FBX %1.%2 %3")
                       .arg(meta.version / 1000)
                       .arg((meta.version / 100) % 10)
                       .arg(meta.ascii ? QObject::tr("ASCII") : QObject::tr("binary"));
    m_doc.detail = str(meta.creator);

    // Nodes, parents first.
    m_nodeMap.assign(m_scene.nodes.count, -1);
    std::vector<int> meshMap(m_scene.meshes.count, -1);
    std::vector<std::pair<const ufbx_node *, int>> stack{{m_scene.root_node, -1}};
    while (!stack.empty()) {
        const auto [src, parent] = stack.back();
        stack.pop_back();
        if (!src || src->typed_id >= m_nodeMap.size() || m_nodeMap[src->typed_id] >= 0)
            continue;
        ModelNode node;
        node.name = str(src->name);
        node.parent = parent;
        const ufbx_transform &t = src->local_transform;
        node.translation = QVector3D(float(t.translation.x), float(t.translation.y), float(t.translation.z));
        node.rotation = QQuaternion(float(t.rotation.w), float(t.rotation.x), float(t.rotation.y), float(t.rotation.z));
        node.scale = QVector3D(float(t.scale.x), float(t.scale.y), float(t.scale.z));
        node.visible = src->visible;
        if (src->mesh)
            node.mesh = int(src->mesh->typed_id); // remapped below
        const int self = int(m_doc.nodes.size());
        m_nodeMap[src->typed_id] = self;
        m_doc.nodes.push_back(std::move(node));
        for (size_t c = src->children.count; c-- > 0;)
            stack.emplace_back(src->children.data[c], self);
    }

    // Skins: one per deformer, joints are the cluster bones.
    std::vector<int> skinMap(m_scene.skin_deformers.count, -1);
    for (size_t s = 0; s < m_scene.skin_deformers.count; ++s) {
        const ufbx_skin_deformer &d = *m_scene.skin_deformers.data[s];
        ModelSkin skin;
        skin.name = str(d.name);
        skin.joints.resize(d.clusters.count, -1);
        skin.inverseBind.resize(d.clusters.count);
        for (size_t c = 0; c < d.clusters.count; ++c) {
            const ufbx_skin_cluster &cl = *d.clusters.data[c];
            if (cl.bone_node && cl.bone_node->typed_id < m_nodeMap.size())
                skin.joints[c] = m_nodeMap[cl.bone_node->typed_id];
            skin.inverseBind[c] = toMatrix(cl.geometry_to_bone);
        }
        skinMap[s] = int(m_doc.skins.size());
        m_doc.skins.push_back(std::move(skin));
    }

    // Materials.
    std::vector<int> materialMap(m_scene.materials.count, -1);
    for (size_t i = 0; i < m_scene.materials.count; ++i) {
        materialMap[i] = int(m_doc.materials.size());
        m_doc.materials.push_back(material(*m_scene.materials.data[i]));
    }

    // Meshes: size bound first (corners are an upper bound on welded
    // vertices), then convert in parallel.
    std::vector<MeshJob> jobs;
    quint64 bound = 0;
    for (size_t i = 0; i < m_scene.meshes.count; ++i) {
        const ufbx_mesh *mesh = m_scene.meshes.data[i];
        if (!mesh->num_triangles || !mesh->instances.count)
            continue;
        MeshJob job;
        job.mesh = mesh;
        if (mesh->skin_deformers.count) {
            const uint32_t id = mesh->skin_deformers.data[0]->typed_id;
            job.skin = id < skinMap.size() ? skinMap[id] : -1;
        }
        for (size_t p = 0; p < mesh->material_parts.count; ++p) {
            const uint32_t mi = mesh->material_parts.data[p].index;
            const ufbx_material *mat = mi < mesh->materials.count ? mesh->materials.data[mi] : nullptr;
            job.partMaterial.push_back(mat && mat->typed_id < materialMap.size() ? materialMap[mat->typed_id] : -1);
        }
        bound += quint64(mesh->num_indices) * (sizeof(ModelVertex) + (job.skin >= 0 ? sizeof(ModelSkinVertex) : 0))
                 + quint64(mesh->num_triangles) * 3 * sizeof(quint32);
        meshMap[i] = int(jobs.size());
        jobs.push_back(std::move(job));
    }
    if (bound > quint64(ModelLoader::kMaxGeometryBytes)) {
        *tooLargeBytes = bound;
        return QString();
    }
    QtConcurrent::blockingMap(jobs, [&cancel](MeshJob &job) { convertMesh(job, cancel); });
    if (cancel.load(std::memory_order_relaxed))
        return QString();

    bool anySkin = false;
    size_t vertices = 0;
    size_t indices = 0;
    for (const MeshJob &job : jobs) {
        if (!job.error.isEmpty())
            return QObject::tr("Could not convert mesh \"%1\": %2").arg(str(job.mesh->name), job.error);
        anySkin = anySkin || job.skin >= 0;
        vertices += job.vertices.size();
        indices += job.indices.size();
    }
    m_doc.vertices.reserve(vertices);
    m_doc.indices.reserve(indices);
    if (anySkin)
        m_doc.skin.reserve(vertices);
    m_doc.meshes.resize(jobs.size());
    for (size_t j = 0; j < jobs.size(); ++j) {
        MeshJob &job = jobs[j];
        ModelMesh &mesh = m_doc.meshes[j];
        mesh.name = str(job.mesh->name);
        mesh.firstPrimitive = int(m_doc.primitives.size());
        const quint32 vbase = quint32(m_doc.vertices.size());
        const quint64 ibase = m_doc.indices.size();
        for (const MeshJob::Part &part : job.parts) {
            ModelPrimitive prim;
            prim.firstIndex = ibase + part.firstIndex;
            prim.indexCount = part.indexCount;
            prim.baseVertex = vbase + part.baseVertex;
            prim.vertexCount = part.vertexCount;
            prim.material = part.material;
            prim.skin = job.skin;
            prim.hasColor = job.mesh->vertex_color.exists;
            m_doc.primitives.push_back(prim);
        }
        mesh.primitiveCount = int(m_doc.primitives.size()) - mesh.firstPrimitive;
        m_doc.vertices.insert(m_doc.vertices.end(), job.vertices.begin(), job.vertices.end());
        if (anySkin) {
            if (job.skin >= 0)
                m_doc.skin.insert(m_doc.skin.end(), job.skinVertices.begin(), job.skinVertices.end());
            else
                m_doc.skin.resize(m_doc.vertices.size());
        }
        m_doc.indices.insert(m_doc.indices.end(), job.indices.begin(), job.indices.end());
        // Free as we go: peak memory stays near one copy.
        std::vector<ModelVertex>().swap(job.vertices);
        std::vector<ModelSkinVertex>().swap(job.skinVertices);
        std::vector<quint32>().swap(job.indices);
    }
    for (ModelNode &node : m_doc.nodes) {
        if (node.mesh >= 0)
            node.mesh = size_t(node.mesh) < meshMap.size() ? meshMap[size_t(node.mesh)] : -1;
    }

    buildAnimations(cancel);
    if (m_scene.blend_deformers.count)
        m_doc.warnings << QObject::tr("blend shapes are not shown");

    m_textures.decode(m_doc, cancel); // embedded content lives in the scene
    return QString();
}

} // namespace

Result load(const QString &path, QByteArrayView bytes, const std::atomic_bool &cancel)
{
    const QByteArray pathUtf8 = path.toUtf8();
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0;
    opts.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
    opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    opts.generate_missing_normals = true;
    opts.clean_skin_weights = true;
    opts.use_blender_pbr_material = true;
    opts.load_external_files = false; // textures are looked up by the viewer, nothing else is followed
    opts.ignore_missing_external_files = true;
    opts.filename.data = pathUtf8.constData();
    opts.filename.length = size_t(pathUtf8.size());
    opts.progress_cb.fn = onProgress;
    opts.progress_cb.user = const_cast<std::atomic_bool *>(&cancel);
    opts.progress_interval_hint = quint64(8) << 20;

    ufbx_error error;
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_memory(bytes.data(), size_t(bytes.size()), &opts, &error), &ufbx_free_scene);
    if (!scene) {
        if (error.type == UFBX_ERROR_CANCELLED || cancel.load(std::memory_order_relaxed))
            return ModelLoader::cancelled();
        if (error.type == UFBX_ERROR_OUT_OF_MEMORY)
            return ModelLoader::failed(QObject::tr("Not enough memory to load this model."));
        return ModelLoader::failed(QObject::tr("Could not read the FBX: %1.").arg(str(error.description)));
    }

    auto doc = std::make_shared<ModelDocument>();
    quint64 tooLargeBytes = 0;
    const QString err = Converter(path, *scene, *doc).run(cancel, &tooLargeBytes);
    if (cancel.load(std::memory_order_relaxed))
        return ModelLoader::cancelled();
    if (tooLargeBytes)
        return ModelLoader::tooLarge(qint64(tooLargeBytes));
    if (!err.isEmpty())
        return ModelLoader::failed(err);
    Result r;
    r.doc = std::move(doc);
    return r;
}

} // namespace FbxLoader
