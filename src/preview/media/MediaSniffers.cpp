#include "MediaSniffers.h"

#include <cstring>

namespace MediaSniffers {

namespace {

bool at(QByteArrayView h, qsizetype offset, const char *magic, qsizetype len)
{
    return h.size() >= offset + len && std::memcmp(h.data() + offset, magic, size_t(len)) == 0;
}

// ISO base media (MP4 / MOV / M4A): a box of type "ftyp" opens the file.
bool isFtyp(QByteArrayView h) { return at(h, 4, "ftyp", 4) && h.size() >= 12; }

bool isAudioBrand(QByteArrayView h)
{
    return at(h, 8, "M4A ", 4) || at(h, 8, "M4B ", 4) || at(h, 8, "M4P ", 4) || at(h, 8, "F4A ", 4)
           || at(h, 8, "F4B ", 4);
}

// The first packet of an Ogg stream names its codec.
bool isOggVideo(QByteArrayView h)
{
    if (!at(h, 0, "OggS", 4) || h.size() < 28)
        return false;
    const qsizetype data = 27 + static_cast<unsigned char>(h[26]);
    return at(h, data, "\x80theora", 7) || at(h, data, "OVP80", 5) || at(h, data, "\x80" "daala", 6);
}

} // namespace

bool isAudio(QByteArrayView h)
{
    if (at(h, 0, "RIFF", 4) || at(h, 0, "RF64", 4))
        return at(h, 8, "WAVE", 4);
    if (at(h, 0, "FORM", 4))
        return at(h, 8, "AIFF", 4) || at(h, 8, "AIFC", 4);
    if (at(h, 0, "OggS", 4))
        return !isOggVideo(h);
    if (isFtyp(h))
        return isAudioBrand(h);
    // ID3v2: "ID3", a version byte below 0xFF and syncsafe size bytes.
    if (at(h, 0, "ID3", 3) && h.size() >= 10) {
        const auto *b = reinterpret_cast<const unsigned char *>(h.data());
        return b[3] < 0xFF && b[4] < 0xFF && (b[6] | b[7] | b[8] | b[9]) < 0x80;
    }
    return at(h, 0, "fLaC", 4) || at(h, 0, "#!AMR", 5) || at(h, 0, "MAC ", 4) || at(h, 0, "wvpk", 4);
}

bool isVideo(QByteArrayView h)
{
    if (isFtyp(h))
        return !isAudioBrand(h);
    if (at(h, 0, "RIFF", 4))
        return at(h, 8, "AVI ", 4);
    // EBML (Matroska / WebM), ASF (WMV), FLV, MPEG program stream.
    return at(h, 0, "\x1A\x45\xDF\xA3", 4) || at(h, 0, "\x30\x26\xB2\x75\x8E\x66\xCF\x11", 8)
           || at(h, 0, "FLV\x01", 4) || at(h, 0, "\x00\x00\x01\xBA", 4) || isOggVideo(h);
}

bool isPdf(QByteArrayView h)
{
    return at(h, 0, "%PDF-", 5);
}

} // namespace MediaSniffers
