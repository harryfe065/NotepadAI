#ifndef HEXSOURCE_H
#define HEXSOURCE_H

#include <QByteArray>
#include <QString>

#include <functional>
#include <vector>

// Bytes of the hex view (add-hex-preview D1): 64 KB pages read on demand into
// a 64-page LRU cache (4 MB). A request reads every missing page of its range
// with one reader call; the file reader opens, reads and closes, so the view
// never holds a handle or a mapping on the file between reads.
//
// GUI thread only; the search runs on its own reader.
class HexSource
{
public:
    // Copies up to `len` bytes at `offset` into `out`; returns the count, or -1
    // when the file cannot be read.
    using Reader = std::function<qint64(qint64 offset, char *out, qint64 len)>;

    static constexpr qint64 kPageBytes = 64 * 1024;
    static constexpr int kMaxPages = 64;

    // Replaces the bytes and drops every cached page.
    void reset(Reader reader, qint64 size);
    qint64 size() const { return m_size; }

    // Bytes [offset, offset + len) clipped to the size; returns the count
    // copied (0 past the end or when the reader fails).
    qint64 read(qint64 offset, char *out, qint64 len);
    // The byte at `offset`, or -1.
    int byteAt(qint64 offset);

    static Reader fileReader(const QString &path);

private:
    struct Page {
        qint64 index = -1;
        QByteArray data;
        quint64 lastUse = 0;
    };

    Page *find(qint64 index);
    bool load(qint64 firstPage, qint64 lastPage);

    Reader m_reader;
    qint64 m_size = 0;
    std::vector<Page> m_pages;
    quint64 m_clock = 0;
};

#endif // HEXSOURCE_H
