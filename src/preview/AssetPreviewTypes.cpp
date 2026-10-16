#include "AssetPreviewTypes.h"

#include "ImagePreviewWidget.h"
#include "LuaState.h"
#include "NotepadNextApplication.h"
#include "PreviewTabManager.h"

#include <QSet>

#include <cstring>
#include <memory>
#include <optional>

namespace {

bool startsWith(QByteArrayView head, const char *magic, qsizetype len)
{
    return head.size() >= len && std::memcmp(head.data(), magic, static_cast<size_t>(len)) == 0;
}

quint32 le32(const unsigned char *p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

bool isPng(QByteArrayView h) { return startsWith(h, "\x89PNG\r\n\x1a\n", 8); }
bool isJpeg(QByteArrayView h) { return startsWith(h, "\xFF\xD8\xFF", 3); }
bool isGif(QByteArrayView h) { return startsWith(h, "GIF87a", 6) || startsWith(h, "GIF89a", 6); }

bool isWebp(QByteArrayView h)
{
    return h.size() >= 12 && startsWith(h, "RIFF", 4) && std::memcmp(h.data() + 8, "WEBP", 4) == 0;
}

// "BM" alone is common in text; also require a sane DIB header size and a
// pixel-data offset inside the claimed file size.
bool isBmp(QByteArrayView h)
{
    if (h.size() < 18 || !startsWith(h, "BM", 2))
        return false;
    const auto *b = reinterpret_cast<const unsigned char *>(h.data());
    const quint32 fileSize = le32(b + 2);
    const quint32 dataOffset = le32(b + 10);
    const quint32 dibSize = le32(b + 14);
    const bool knownDib = dibSize == 12 || dibSize == 40 || dibSize == 52 || dibSize == 56
                          || dibSize == 64 || dibSize == 108 || dibSize == 124;
    return knownDib && dataOffset >= 14 + dibSize && (fileSize == 0 || dataOffset < fileSize);
}

// An SVG without the extension: optional BOM, XML prolog, comments or a
// doctype, then the <svg root, all inside the head.
bool isSvg(QByteArrayView h)
{
    qsizetype i = 0;
    const qsizetype n = h.size();
    if (n >= 3 && static_cast<unsigned char>(h[0]) == 0xEF && static_cast<unsigned char>(h[1]) == 0xBB
        && static_cast<unsigned char>(h[2]) == 0xBF)
        i = 3;
    while (i < n) {
        const char c = h[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
            continue;
        }
        if (c != '<')
            return false;
        const QByteArrayView rest = h.sliced(i);
        if (rest.startsWith("<svg"))
            return rest.size() > 4 && (rest[4] == ' ' || rest[4] == '>' || rest[4] == '\t' || rest[4] == '\r'
                                       || rest[4] == '\n' || rest[4] == '/');
        // Skip <?…?>, <!--…-->, <!DOCTYPE …>; anything else is another root.
        const char *close = rest.startsWith("<?") ? "?>" : rest.startsWith("<!--") ? "-->"
                            : rest.startsWith("<!") ? ">" : nullptr;
        if (!close)
            return false;
        const qsizetype end = rest.indexOf(QByteArrayView(close));
        if (end < 0)
            return false;
        i += end + qsizetype(std::strlen(close));
    }
    return false;
}

PreviewTabManager::TypeRegistration imageRegistration(const QString &typeId, const QString &displayName,
                                                      QStringList extensions)
{
    PreviewTabManager::TypeRegistration reg;
    reg.extensions = std::move(extensions);
    reg.iconPath = QStringLiteral(":/icons/image-preview.svg");
    reg.factory = [typeId, displayName](QWidget *parent) -> PreviewContentWidget * {
        return new ImagePreviewWidget(typeId, displayName, parent);
    };
    reg.wantsFilePath = true;
    reg.displayName = displayName;
    reg.opensByDefault = true;
    reg.watchFile = true;
    return reg;
}

} // namespace

void registerAssetPreviewTypes(PreviewTabManager *manager, NotepadNextApplication *app)
{
    auto image = imageRegistration(
        QStringLiteral("image"), QStringLiteral("Image"),
        {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jpe"),
         QStringLiteral("jfif"), QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("dib"),
         QStringLiteral("webp"), QStringLiteral("ico"), QStringLiteral("cur"), QStringLiteral("tif"),
         QStringLiteral("tiff"), QStringLiteral("tga"), QStringLiteral("pbm"), QStringLiteral("pgm"),
         QStringLiteral("ppm"), QStringLiteral("xbm"), QStringLiteral("xpm")});
    image.sniffers = {isPng, isJpeg, isGif, isWebp, isBmp};
    manager->registerType(QStringLiteral("image"), image);

    auto svg = imageRegistration(QStringLiteral("svg"), QStringLiteral("SVG"),
                                 {QStringLiteral("svg"), QStringLiteral("svgz")});
    svg.sniffers = {isSvg};
    manager->registerType(QStringLiteral("svg"), svg);

    // Known text extensions skip the head read entirely. Snapshotted from the
    // Lua language table on first use (the table is loaded by then and never
    // changes at runtime); "" (text.lua's fallback) stays unknown so bare
    // files are still sniffed.
    auto textExtensions = std::make_shared<std::optional<QSet<QString>>>();
    manager->resolver().setTextExtensionProbe([app, textExtensions](const QString &extLower) {
        if (!textExtensions->has_value()) {
            const QStringList all = app->getLuaState()->executeAndReturn<QStringList>(R"(
                local out = {}
                for _, L in pairs(languages) do
                    if L.extensions then
                        for _, v in ipairs(L.extensions) do
                            if v ~= "" then out[#out + 1] = string.lower(v) end
                        end
                    end
                end
                return out
            )");
            textExtensions->emplace(all.cbegin(), all.cend());
        }
        return (*textExtensions)->contains(extLower);
    });
}
