#pragma once

// Small Wii U content files written byte by byte, independent of the code that reads them.
#include "wiiuport/content/BigEndian.h"
#include "wiiuport/content/LayoutPanes.h"
#include "wiiuport/content/SarcArchive.h"
#include "wiiuport/content/SurfaceTiling.h"
#include "wiiuport/content/Yaz0.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace wiiuport::tests::fixtures {

inline constexpr size_t kSheetData = 0x100;

// Row-major RGBA8, standing in for the fork's GX2 address library.
inline content::SurfaceTiling linearTiling() {
    return [](const content::SurfaceShape& shape) {
        return [width = shape.width](uint32_t x, uint32_t y) {
            return ((y * width) + x) * 4;
        };
    };
}

inline void put(std::vector<uint8_t>& bytes, size_t at, const char* magic) {
    std::memcpy(&bytes[at], magic, 4);
}

// One RGBA8 sheet of cellsPerRow x cellsPerColumn cells, the code points mapped in order by a scan
// CMAP. The sheet is as wide and high as the grid with its one-pixel lines.
inline std::vector<uint8_t> pictureFont(uint32_t cell, uint32_t perRow, uint32_t perColumn,
                                        const std::vector<char32_t>& codes) {
    const uint32_t width = 1 + (perRow * (cell + 1));
    const uint32_t height = 1 + (perColumn * (cell + 1));
    const uint32_t sheetSize = width * height * 4;
    std::vector<uint8_t> font(kSheetData + sheetSize, 0);
    put(font, 0, "FFNT");
    content::writeU16(font, 4, 0xfeff);
    content::writeU16(font, 6, 0x14);
    content::writeU32(font, 8, 0x03000000);
    content::writeU32(font, 0xc, static_cast<uint32_t>(font.size()));
    put(font, 0x14, "FINF");
    content::writeU32(font, 0x18, 0x20);
    content::writeU32(font, 0x14 + 0x14, 0x34 + 8);
    content::writeU32(font, 0x14 + 0x1c, 0x54 + 8);
    put(font, 0x34, "TGLP");
    content::writeU32(font, 0x38, 0x20);
    font[0x3c] = static_cast<uint8_t>(cell);
    font[0x3d] = static_cast<uint8_t>(cell);
    font[0x3e] = 1;
    content::writeU32(font, 0x40, sheetSize);
    content::writeU16(font, 0x46, 0xe);
    content::writeU16(font, 0x48, static_cast<uint16_t>(perRow));
    content::writeU16(font, 0x4a, static_cast<uint16_t>(perColumn));
    content::writeU16(font, 0x4c, static_cast<uint16_t>(width));
    content::writeU16(font, 0x4e, static_cast<uint16_t>(height));
    content::writeU32(font, 0x50, kSheetData);
    put(font, 0x54, "CMAP");
    content::writeU32(font, 0x58, static_cast<uint32_t>(0x16 + (4 * codes.size())));
    content::writeU16(font, 0x5c, 0);
    content::writeU16(font, 0x5e, 0xffff);
    content::writeU16(font, 0x60, 2);
    content::writeU16(font, 0x68, static_cast<uint16_t>(codes.size()));
    for (size_t index = 0; index < codes.size(); ++index) {
        content::writeU16(font, 0x6a + (4 * index), static_cast<uint16_t>(codes[index]));
        content::writeU16(font, 0x6c + (4 * index), static_cast<uint16_t>(index));
    }
    return font;
}

inline std::vector<uint8_t> pictureFont(uint32_t cell, uint32_t perRow, uint32_t perColumn) {
    return pictureFont(cell, perRow, perColumn, {U'', U''});
}

struct LayoutText {
    std::string pane;
    std::u16string text;
};

inline content::LayoutPanes::Color whiteOf(const std::vector<uint8_t>& layout,
                                           const std::string& material) {
    const size_t mat = 0x14;
    const uint16_t count = content::readU16(layout, mat + 8);
    for (uint16_t index = 0; index < count; ++index) {
        const size_t at = mat + content::readU32(layout, mat + 0xc + (4 * size_t{index}));
        if (std::string(reinterpret_cast<const char*>(&layout[at])) == material) {
            return {layout[at + 0x20], layout[at + 0x21], layout[at + 0x22], layout[at + 0x23]};
        }
    }
    return {};
}

// A layout of a material and a text pane per entry, as the title names a pane's material after it;
// each box 30 wide, each material white.
inline std::vector<uint8_t> layoutOf(const std::vector<LayoutText>& texts) {
    std::vector<uint8_t> bytes(0x14, 0);
    put(bytes, 0, "FLYT");
    content::writeU16(bytes, 4, 0xfeff);
    content::writeU16(bytes, 6, 0x14);
    content::writeU32(bytes, 8, 0x02020000);
    const size_t mat = bytes.size();
    const size_t materialSize = 0x28;
    bytes.resize(mat + 0xc + ((4 + materialSize) * texts.size()), 0);
    put(bytes, mat, "mat1");
    content::writeU32(bytes, mat + 4, static_cast<uint32_t>(bytes.size() - mat));
    content::writeU16(bytes, mat + 8, static_cast<uint16_t>(texts.size()));
    for (size_t index = 0; index < texts.size(); ++index) {
        const size_t offset = 0xc + (4 * texts.size()) + (materialSize * index);
        content::writeU32(bytes, mat + 0xc + (4 * index), static_cast<uint32_t>(offset));
        std::memcpy(&bytes[mat + offset], texts[index].pane.data(), texts[index].pane.size());
        content::writeU32(bytes, mat + offset + 0x20, 0xffffffff);
    }
    for (const LayoutText& text : texts) {
        const size_t at = bytes.size();
        const auto buffer = static_cast<uint16_t>((text.text.size() + 1) * 2);
        bytes.resize(at + ((0xa0 + buffer + 3) & ~3U), 0);
        put(bytes, at, "txt1");
        content::writeU32(bytes, at + 4, static_cast<uint32_t>(bytes.size() - at));
        std::memcpy(&bytes[at + 0xc], text.pane.data(), text.pane.size());
        content::writeU32(bytes, at + 0x4c, 0x41f00000);
        content::writeU16(bytes, at + 0x54, buffer);
        content::writeU16(bytes, at + 0x56, buffer);
        content::writeU32(bytes, at + 0x64, 0xa0);
        for (size_t i = 0; i < text.text.size(); ++i) {
            content::writeU16(bytes, at + 0xa0 + (2 * i), static_cast<uint16_t>(text.text[i]));
        }
    }
    content::writeU32(bytes, 0xc, static_cast<uint32_t>(bytes.size()));
    content::writeU16(bytes, 0x10, static_cast<uint16_t>(1 + texts.size()));
    return bytes;
}

inline void padSixteen(std::vector<uint8_t>& bytes) {
    bytes.resize((bytes.size() + 15) & ~size_t{15}, 0xab);
}

// An MSBT with one label group, each label's message at its index.
inline std::vector<uint8_t> messagesOf(const std::vector<LayoutText>& messages) {
    std::vector<uint8_t> bytes(0x20, 0);
    std::memcpy(bytes.data(), "MsgStdBn", 8);
    content::writeU16(bytes, 8, 0xfeff);
    bytes[0xc] = 1;
    bytes[0xd] = 3;
    content::writeU16(bytes, 0xe, 2);
    const size_t labels = bytes.size();
    bytes.resize(labels + 0x10 + 4 + 8, 0);
    put(bytes, labels, "LBL1");
    content::writeU32(bytes, labels + 0x10, 1);
    content::writeU32(bytes, labels + 0x14, static_cast<uint32_t>(messages.size()));
    content::writeU32(bytes, labels + 0x18, 12);
    for (size_t index = 0; index < messages.size(); ++index) {
        bytes.push_back(static_cast<uint8_t>(messages[index].pane.size()));
        bytes.insert(bytes.end(), messages[index].pane.begin(), messages[index].pane.end());
        bytes.resize(bytes.size() + 4, 0);
        content::writeU32(bytes, bytes.size() - 4, static_cast<uint32_t>(index));
    }
    content::writeU32(bytes, labels + 4, static_cast<uint32_t>(bytes.size() - labels - 0x10));
    padSixteen(bytes);
    const size_t texts = bytes.size();
    bytes.resize(texts + 0x10 + 4 + (4 * messages.size()), 0);
    put(bytes, texts, "TXT2");
    content::writeU32(bytes, texts + 0x10, static_cast<uint32_t>(messages.size()));
    for (size_t index = 0; index < messages.size(); ++index) {
        content::writeU32(bytes, texts + 0x14 + (4 * index),
                          static_cast<uint32_t>(bytes.size() - texts - 0x10));
        for (char16_t unit : messages[index].text) {
            bytes.push_back(static_cast<uint8_t>(unit >> 8));
            bytes.push_back(static_cast<uint8_t>(unit));
        }
        bytes.push_back(0);
        bytes.push_back(0);
    }
    content::writeU32(bytes, texts + 4, static_cast<uint32_t>(bytes.size() - texts - 0x10));
    padSixteen(bytes);
    content::writeU32(bytes, 0x12, static_cast<uint32_t>(bytes.size()));
    return bytes;
}

// A SARC of the named files, each at `alignment`, with the given hash for each name's order.
inline std::vector<uint8_t>
sarcBytes(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
          size_t alignment) {
    std::vector<uint8_t> names;
    std::vector<size_t> nameAt;
    for (const auto& file : files) {
        nameAt.push_back(names.size());
        names.insert(names.end(), file.first.begin(), file.first.end());
        names.resize((names.size() + 4) & ~size_t{3}, 0);
    }
    const size_t fnt = 0x14 + 0xc + (0x10 * files.size());
    size_t data = (fnt + 8 + names.size() + alignment - 1) / alignment * alignment;
    std::vector<size_t> starts;
    size_t cursor = data;
    for (const auto& file : files) {
        cursor = (cursor + alignment - 1) / alignment * alignment;
        starts.push_back(cursor - data);
        cursor += file.second.size();
    }
    std::vector<uint8_t> bytes(cursor, 0);
    put(bytes, 0, "SARC");
    content::writeU16(bytes, 4, 0x14);
    content::writeU16(bytes, 6, 0xfeff);
    content::writeU32(bytes, 8, static_cast<uint32_t>(bytes.size()));
    content::writeU32(bytes, 0xc, static_cast<uint32_t>(data));
    put(bytes, 0x14, "SFAT");
    content::writeU16(bytes, 0x18, 0xc);
    content::writeU16(bytes, 0x1a, static_cast<uint16_t>(files.size()));
    content::writeU32(bytes, 0x1c, 0x65);
    for (size_t index = 0; index < files.size(); ++index) {
        const size_t node = 0x20 + (0x10 * index);
        content::writeU32(bytes, node, static_cast<uint32_t>(index));
        content::writeU32(bytes, node + 4, 0x01000000U | static_cast<uint32_t>(nameAt[index] / 4));
        content::writeU32(bytes, node + 8, static_cast<uint32_t>(starts[index]));
        content::writeU32(bytes, node + 12,
                          static_cast<uint32_t>(starts[index] + files[index].second.size()));
        std::memcpy(&bytes[data + starts[index]], files[index].second.data(),
                    files[index].second.size());
    }
    put(bytes, fnt, "SFNT");
    content::writeU16(bytes, fnt + 4, 8);
    std::memcpy(&bytes[fnt + 8], names.data(), names.size());
    return bytes;
}

inline content::SarcArchive
archiveOf(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
          size_t alignment) {
    std::string refusal;
    return *content::SarcArchive::parse(sarcBytes(files, alignment), refusal);
}

inline std::vector<uint8_t>
yaz0Sarc(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    return content::Yaz0::encode(sarcBytes(files, 0x100), 0x2000);
}

} // namespace wiiuport::tests::fixtures
