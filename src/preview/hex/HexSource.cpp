#include "HexSource.h"

#include <QFile>

#include <algorithm>
#include <cstring>

void HexSource::reset(Reader reader, qint64 size)
{
    m_reader = std::move(reader);
    m_size = std::max<qint64>(0, size);
    m_pages.clear();
    m_clock = 0;
}

HexSource::Page *HexSource::find(qint64 index)
{
    // 64 entries: a linear scan beats any hashing here.
    for (Page &page : m_pages) {
        if (page.index == index) {
            page.lastUse = ++m_clock;
            return &page;
        }
    }
    return nullptr;
}

bool HexSource::load(qint64 firstPage, qint64 lastPage)
{
    const qint64 offset = firstPage * kPageBytes;
    const qint64 length = std::min(m_size, (lastPage + 1) * kPageBytes) - offset;
    QByteArray buffer(length, Qt::Uninitialized);
    const qint64 got = m_reader ? m_reader(offset, buffer.data(), length) : -1;
    if (got <= 0)
        return false;
    for (qint64 index = firstPage; index <= lastPage; ++index) {
        const qint64 start = (index - firstPage) * kPageBytes;
        if (start >= got)
            break;
        // A cached page inside the run is refreshed in place, never duplicated.
        Page *slot = find(index);
        if (!slot) {
            if (qsizetype(m_pages.size()) < kMaxPages) {
                m_pages.emplace_back();
                slot = &m_pages.back();
            } else {
                slot = &*std::min_element(m_pages.begin(), m_pages.end(),
                                          [](const Page &a, const Page &b) { return a.lastUse < b.lastUse; });
            }
        }
        slot->index = index;
        slot->data = buffer.mid(start, std::min(kPageBytes, got - start));
        slot->lastUse = ++m_clock;
    }
    return true;
}

qint64 HexSource::read(qint64 offset, char *out, qint64 len)
{
    if (offset < 0 || offset >= m_size || len <= 0)
        return 0;
    len = std::min(len, m_size - offset);
    const qint64 first = offset / kPageBytes;
    const qint64 last = (offset + len - 1) / kPageBytes;

    // One reader call for the run of missing pages (usually one or two).
    qint64 missingFirst = -1, missingLast = -1;
    for (qint64 index = first; index <= last; ++index) {
        if (!find(index)) {
            if (missingFirst < 0)
                missingFirst = index;
            missingLast = index;
        }
    }
    // A request wider than the cache can still be served: it is loaded and
    // copied in cache-sized slices.
    if (missingFirst >= 0 && missingLast - missingFirst + 1 > kMaxPages) {
        qint64 copied = 0;
        while (copied < len) {
            const qint64 slice = std::min(len - copied, kPageBytes * (kMaxPages / 2));
            const qint64 got = read(offset + copied, out + copied, slice);
            if (got <= 0)
                break;
            copied += got;
        }
        return copied;
    }
    if (missingFirst >= 0 && !load(missingFirst, missingLast))
        return 0;

    qint64 copied = 0;
    for (qint64 index = first; index <= last; ++index) {
        const Page *page = find(index);
        if (!page)
            break;
        const qint64 pageStart = index * kPageBytes;
        const qint64 from = std::max(offset, pageStart) - pageStart;
        const qint64 to = std::min(offset + len, pageStart + qint64(page->data.size())) - pageStart;
        if (to <= from)
            break;
        std::memcpy(out + copied, page->data.constData() + from, size_t(to - from));
        copied += to - from;
    }
    return copied;
}

int HexSource::byteAt(qint64 offset)
{
    char c = 0;
    return read(offset, &c, 1) == 1 ? int(quint8(c)) : -1;
}

HexSource::Reader HexSource::fileReader(const QString &path)
{
    return [path](qint64 offset, char *out, qint64 len) -> qint64 {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || !file.seek(offset))
            return -1;
        return file.read(out, len);
    };
}
