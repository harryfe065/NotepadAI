#ifndef GLTFLOADER_H
#define GLTFLOADER_H

#include "ModelLoader.h"

// glTF 2.0 (.gltf with external / data-URI buffers, .glb) through cgltf.
// External buffers are memory-mapped; primitives convert in parallel.
namespace GltfLoader {

ModelLoader::Result load(const QString &path, QByteArrayView bytes, const std::atomic_bool &cancel);

} // namespace GltfLoader

#endif // GLTFLOADER_H
