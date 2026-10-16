#include "FileKindResolver.h"

#include <QFile>
#include <QFileInfo>

#include <cstring>

void FileKindResolver::addPreviewExtensions(const QString &typeId, const QStringList &extensions)
{
    for (const QString &ext : extensions)
        m_previewByExt.insert(ext.toLower(), typeId);
}

void FileKindResolver::addSniffer(const QString &typeId, Sniffer sniffer)
{
    if (sniffer)
        m_sniffers.append({typeId, std::move(sniffer)});
}

void FileKindResolver::setTextExtensionProbe(std::function<bool(const QString &)> probe)
{
    m_isTextExtension = std::move(probe);
}

FileKindResolver::Result FileKindResolver::resolveByExtension(const QString &suffixLower, bool *known) const
{
    if (!suffixLower.isEmpty()) {
        const auto it = m_previewByExt.constFind(suffixLower);
        if (it != m_previewByExt.constEnd()) {
            if (known) *known = true;
            return {Kind::Preview, it.value()};
        }
        if (m_isTextExtension && m_isTextExtension(suffixLower)) {
            if (known) *known = true;
            return {Kind::Text, {}};
        }
    }
    if (known) *known = false;
    return {Kind::Text, {}};
}

FileKindResolver::Result FileKindResolver::classifyHead(QByteArrayView head) const
{
    if (head.isEmpty())
        return {Kind::Text, {}};

    for (const auto &sniffer : m_sniffers) {
        if (sniffer.second(head))
            return {Kind::Preview, sniffer.first};
    }

    const auto *b = reinterpret_cast<const unsigned char *>(head.data());
    const qsizetype n = head.size();
    // A BOM marks text even though UTF-16/32 text is full of NUL bytes.
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF)
        return {Kind::Text, {}};
    if (n >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF)))
        return {Kind::Text, {}};

    if (std::memchr(head.data(), 0, static_cast<size_t>(n)) != nullptr)
        return {Kind::Binary, {}};

    return {Kind::Text, {}};
}

FileKindResolver::Result FileKindResolver::resolve(const QString &filePath) const
{
    bool known = false;
    Result byExt = resolveByExtension(QFileInfo(filePath).suffix().toLower(), &known);
    if (known)
        return byExt;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return {Kind::Text, {}};
    char head[kHeadBytes];
    const qint64 got = file.read(head, kHeadBytes);
    if (got <= 0)
        return {Kind::Text, {}};
    return classifyHead(QByteArrayView(head, static_cast<qsizetype>(got)));
}
