#ifndef ASSETFILEUTIL_H
#define ASSETFILEUTIL_H

#include <QByteArrayView>
#include <QStringView>

// Small helpers shared by the tile map and atlas loaders.
namespace AssetFileUtil {

// A file head past an optional UTF-8 BOM and leading whitespace.
inline QByteArrayView trimmedHead(QByteArrayView head)
{
    if (head.size() >= 3 && static_cast<unsigned char>(head[0]) == 0xEF && static_cast<unsigned char>(head[1]) == 0xBB
        && static_cast<unsigned char>(head[2]) == 0xBF)
        head = head.sliced(3);
    qsizetype i = 0;
    while (i < head.size() && (head[i] == ' ' || head[i] == '\t' || head[i] == '\r' || head[i] == '\n'))
        ++i;
    return head.sliced(i);
}

// True for a path written in a file that names a network location
// (\\host\share, //host/share, \\?\UNC\..., \\.\device, mixed slashes alike).
// Loaders refuse such references from file contents, so probing a crafted
// file never makes Windows connect over SMB (and leak credentials).
inline bool isNetworkPath(QStringView path)
{
    return path.size() >= 2 && (path[0] == u'\\' || path[0] == u'/') && (path[1] == u'\\' || path[1] == u'/');
}

} // namespace AssetFileUtil

#endif // ASSETFILEUTIL_H
