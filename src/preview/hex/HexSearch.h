#ifndef HEXSEARCH_H
#define HEXSEARCH_H

#include "HexSource.h"

#include <QByteArrayView>

#include <atomic>

// Byte-pattern search of the hex view (add-hex-preview D2). Scans the file in
// chunks that overlap by pattern length - 1 (a match across a chunk border is
// found), with Boyer–Moore–Horspool forward and over reversed iterators
// backward. Cancellation is checked between chunks. Runs on any thread with
// its own reader.
namespace HexSearch {

constexpr qint64 kChunkBytes = 4 * 1024 * 1024;
constexpr qint64 kNotFound = -1;
constexpr qint64 kCancelled = -2;
constexpr qint64 kReadError = -3;

// Forward: the first match starting in [from, to). Backward: the last match
// starting in [from, to). A match may run past `to`, never past `size`.
qint64 find(const HexSource::Reader &read, qint64 size, QByteArrayView pattern, qint64 from, qint64 to,
            bool backward, const std::atomic_bool &cancel, qint64 chunkBytes = kChunkBytes);

} // namespace HexSearch

#endif // HEXSEARCH_H
