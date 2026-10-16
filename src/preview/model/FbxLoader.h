#ifndef FBXLOADER_H
#define FBXLOADER_H

#include "ModelLoader.h"

// Autodesk FBX (binary and ASCII, 6.x and 7.x) through ufbx, converted to
// right-handed Y-up metres. Meshes convert in parallel, one primitive per
// material, de-duplicated with ufbx_generate_indices; skins become joint
// matrices, animation stacks are baked to linear keys.
namespace FbxLoader {

ModelLoader::Result load(const QString &path, QByteArrayView bytes, const std::atomic_bool &cancel);

} // namespace FbxLoader

#endif // FBXLOADER_H
