#ifndef SIMPLEMESHLOADERS_H
#define SIMPLEMESHLOADERS_H

#include "ModelLoader.h"

// Formats simple enough for an in-house parser, all over the mapped file:
// - STL: binary (decoded in parallel, VisCAM / Materialise facet colours)
//   and ASCII; triangle soup drawn without an index buffer, facet normals
//   from the shader. Z-up.
// - PLY: ASCII and binary (both endians); positions, normals, colours, UVs,
//   polygon lists (fan triangulated) or a point cloud when there are no faces.
// - VOX (MagicaVoxel): models greedy-meshed into quads, palette colours as
//   vertex colours, MATL metal / glass / emit, the nTRN/nGRP/nSHP scene graph
//   and hidden layers. Z-up.
namespace SimpleMeshLoaders {

ModelLoader::Result loadStl(QByteArrayView bytes, const std::atomic_bool &cancel);
ModelLoader::Result loadPly(QByteArrayView bytes, const std::atomic_bool &cancel);
ModelLoader::Result loadVox(QByteArrayView bytes, const std::atomic_bool &cancel);

} // namespace SimpleMeshLoaders

#endif // SIMPLEMESHLOADERS_H
