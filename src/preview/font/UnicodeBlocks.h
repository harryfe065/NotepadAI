#ifndef UNICODEBLOCKS_H
#define UNICODEBLOCKS_H

#include <QString>

#include <vector>

// The Unicode blocks the font preview offers, in code point order. A grid of
// all 0x110000 code points would be 70 000 rows of mostly nothing; a block
// picker shows only ranges worth looking at.
namespace UnicodeBlocks {

struct Block {
    const char *name;
    quint32 first;
    quint32 last;       // inclusive
};

const std::vector<Block> &all();

// Index of the block holding `codePoint`, or -1.
int indexOf(quint32 codePoint);

} // namespace UnicodeBlocks

#endif // UNICODEBLOCKS_H
