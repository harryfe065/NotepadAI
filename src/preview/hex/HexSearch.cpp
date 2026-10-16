#include "HexSearch.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <vector>

namespace HexSearch {

qint64 find(const HexSource::Reader &read, qint64 size, QByteArrayView pattern, qint64 from, qint64 to,
            bool backward, const std::atomic_bool &cancel, qint64 chunkBytes)
{
    const qint64 m = pattern.size();
    from = std::max<qint64>(0, from);
    to = std::min(to, size);
    if (m == 0 || from >= to || m > size || !read)
        return kNotFound;
    chunkBytes = std::max<qint64>(chunkBytes, 1);
    std::vector<char> buffer(size_t(chunkBytes + m - 1));

    // Reads the window of match starts [start, end): its bytes run to
    // end + m - 1 (clipped to the size), so every match in it is complete.
    auto window = [&](qint64 start, qint64 end) -> qint64 {
        const qint64 length = std::min(size, end + m - 1) - start;
        return length >= m ? read(start, buffer.data(), length) : 0;
    };

    if (!backward) {
        const std::boyer_moore_horspool_searcher searcher(pattern.begin(), pattern.end());
        for (qint64 start = from; start < to; start += chunkBytes) {
            if (cancel.load(std::memory_order_relaxed))
                return kCancelled;
            const qint64 end = std::min(to, start + chunkBytes);
            const qint64 got = window(start, end);
            if (got < 0)
                return kReadError;
            const auto first = buffer.cbegin(), last = buffer.cbegin() + got;
            const auto hit = std::search(first, last, searcher);
            if (hit != last)
                return start + (hit - first);
        }
        return kNotFound;
    }

    // Backward: the same searcher over the reversed pattern and window finds
    // the last match of each window first.
    const auto rpBegin = std::make_reverse_iterator(pattern.end());
    const auto rpEnd = std::make_reverse_iterator(pattern.begin());
    const std::boyer_moore_horspool_searcher searcher(rpBegin, rpEnd);
    for (qint64 end = to; end > from; end -= chunkBytes) {
        if (cancel.load(std::memory_order_relaxed))
            return kCancelled;
        const qint64 start = std::max(from, end - chunkBytes);
        const qint64 got = window(start, end);
        if (got < 0)
            return kReadError;
        const auto first = std::make_reverse_iterator(buffer.cbegin() + got);
        const auto last = std::make_reverse_iterator(buffer.cbegin());
        const auto hit = std::search(first, last, searcher);
        if (hit != last) {
            // hit.base() is one past the match's last byte.
            const qint64 matchEnd = hit.base() - buffer.cbegin();
            return start + matchEnd - m;
        }
        if (start == from)
            break;
    }
    return kNotFound;
}

} // namespace HexSearch
