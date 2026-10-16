#ifndef OBJLOADER_H
#define OBJLOADER_H

#include "ModelLoader.h"

// Wavefront OBJ + MTL through fast_obj (streamed in chunks; cancellable).
// Corners are welded with a per-position chain (no hash table: memory stays
// at 4 bytes per position plus 12 per vertex), one primitive per material.
namespace ObjLoader {

ModelLoader::Result load(const QString &path, const std::atomic_bool &cancel);

} // namespace ObjLoader

#endif // OBJLOADER_H
