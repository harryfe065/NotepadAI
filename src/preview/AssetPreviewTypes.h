#ifndef ASSETPREVIEWTYPES_H
#define ASSETPREVIEWTYPES_H

class PreviewTabManager;
class NotepadNextApplication;

// Registers every asset preview type (images, SVG; later milestones add
// textures, models, tile maps, media and hex) and the text-extension probe the
// shared FileKindResolver uses. Call once, after the Markdown/HTML/CSV types.
void registerAssetPreviewTypes(PreviewTabManager *manager, NotepadNextApplication *app);

#endif // ASSETPREVIEWTYPES_H
