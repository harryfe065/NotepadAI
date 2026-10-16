#include "UnicodeBlocks.h"

#include <algorithm>
#include <iterator>

namespace UnicodeBlocks {

const std::vector<Block> &all()
{
    static const std::vector<Block> blocks = {
        {"Basic Latin", 0x0020, 0x007F},
        {"Latin-1 Supplement", 0x0080, 0x00FF},
        {"Latin Extended-A", 0x0100, 0x017F},
        {"Latin Extended-B", 0x0180, 0x024F},
        {"IPA Extensions", 0x0250, 0x02AF},
        {"Greek and Coptic", 0x0370, 0x03FF},
        {"Cyrillic", 0x0400, 0x04FF},
        {"Armenian", 0x0530, 0x058F},
        {"Hebrew", 0x0590, 0x05FF},
        {"Arabic", 0x0600, 0x06FF},
        {"Devanagari", 0x0900, 0x097F},
        {"Bengali", 0x0980, 0x09FF},
        {"Thai", 0x0E00, 0x0E7F},
        {"Georgian", 0x10A0, 0x10FF},
        {"Hangul Jamo", 0x1100, 0x11FF},
        {"Latin Extended Additional", 0x1E00, 0x1EFF},
        {"Greek Extended", 0x1F00, 0x1FFF},
        {"General Punctuation", 0x2000, 0x206F},
        {"Superscripts and Subscripts", 0x2070, 0x209F},
        {"Currency Symbols", 0x20A0, 0x20CF},
        {"Letterlike Symbols", 0x2100, 0x214F},
        {"Number Forms", 0x2150, 0x218F},
        {"Arrows", 0x2190, 0x21FF},
        {"Mathematical Operators", 0x2200, 0x22FF},
        {"Miscellaneous Technical", 0x2300, 0x23FF},
        {"Box Drawing", 0x2500, 0x257F},
        {"Block Elements", 0x2580, 0x259F},
        {"Geometric Shapes", 0x25A0, 0x25FF},
        {"Miscellaneous Symbols", 0x2600, 0x26FF},
        {"Dingbats", 0x2700, 0x27BF},
        {"Braille Patterns", 0x2800, 0x28FF},
        {"CJK Symbols and Punctuation", 0x3000, 0x303F},
        {"Hiragana", 0x3040, 0x309F},
        {"Katakana", 0x30A0, 0x30FF},
        {"CJK Unified Ideographs", 0x4E00, 0x9FFF},
        {"Hangul Syllables", 0xAC00, 0xD7AF},
        {"Private Use Area", 0xE000, 0xF8FF},
        {"Alphabetic Presentation Forms", 0xFB00, 0xFB4F},
        {"Halfwidth and Fullwidth Forms", 0xFF00, 0xFFEF},
        {"Miscellaneous Symbols and Pictographs", 0x1F300, 0x1F5FF},
        {"Emoticons", 0x1F600, 0x1F64F},
        {"Transport and Map Symbols", 0x1F680, 0x1F6FF},
        {"Supplemental Symbols and Pictographs", 0x1F900, 0x1F9FF},
    };
    return blocks;
}

int indexOf(quint32 codePoint)
{
    // Blocks are sorted and disjoint: the last block starting at or before
    // the code point is the only candidate.
    const std::vector<Block> &b = all();
    const auto it = std::upper_bound(b.begin(), b.end(), codePoint,
                                     [](quint32 cp, const Block &block) { return cp < block.first; });
    if (it == b.begin())
        return -1;
    const auto candidate = std::prev(it);
    return codePoint <= candidate->last ? int(candidate - b.begin()) : -1;
}

} // namespace UnicodeBlocks
