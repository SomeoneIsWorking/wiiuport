#include "wiiuport/content/FontSheets.h"

#include "wiiuport/content/BigEndian.h"

#include <algorithm>
#include <cstring>

namespace wiiuport::content {
namespace {

constexpr uint32_t kSheetRgba8 = 0;
constexpr uint32_t kSheetRgba8Srgb = 0xe;
constexpr uint32_t kSheetFormatMask = 0x3fff;
constexpr size_t kBlockHeader = 8;

bool magicAt(std::span<const uint8_t> bytes, size_t at, const char* magic) {
    return at + 4 <= bytes.size() && std::memcmp(&bytes[at], magic, 4) == 0;
}

} // namespace

std::optional<FontSheets> FontSheets::parse(std::span<uint8_t> font, std::string& refusal) {
    if (!magicAt(font, 0, "FFNT") || font.size() < 0x14 || readU16(font, 4) != 0xfeff) {
        refusal = "not a big-endian FFNT font";
        return std::nullopt;
    }
    const size_t info = readU16(font, 6);
    if (!magicAt(font, info, "FINF") || info + 0x20 > font.size()) {
        refusal = "no FINF block";
        return std::nullopt;
    }
    const size_t glyphs = readU32(font, info + 0x14) - kBlockHeader;
    if (!magicAt(font, glyphs, "TGLP") || glyphs + 0x20 > font.size()) {
        refusal = "no TGLP block";
        return std::nullopt;
    }
    FontSheets sheets;
    sheets.m_font = font;
    sheets.m_cellWidth = font[glyphs + 8];
    sheets.m_cellHeight = font[glyphs + 9];
    sheets.m_sheetCount = font[glyphs + 0xa];
    sheets.m_sheetSize = readU32(font, glyphs + 0xc);
    const uint32_t format = readU16(font, glyphs + 0x12) & kSheetFormatMask;
    sheets.m_cellsPerRow = readU16(font, glyphs + 0x14);
    sheets.m_cellsPerColumn = readU16(font, glyphs + 0x16);
    sheets.m_sheetWidth = readU16(font, glyphs + 0x18);
    sheets.m_sheetHeight = readU16(font, glyphs + 0x1a);
    sheets.m_sheetData = readU32(font, glyphs + 0x1c);
    if (format == kSheetRgba8) {
        sheets.m_gx2Format = kGx2Rgba8;
    } else if (format == kSheetRgba8Srgb) {
        sheets.m_gx2Format = kGx2Rgba8Srgb;
    } else {
        refusal = "sheet format " + std::to_string(format) + " is not RGBA8";
        return std::nullopt;
    }
    if (size_t{sheets.m_sheetData} + (size_t{sheets.m_sheetSize} * sheets.m_sheetCount) >
            font.size() ||
        size_t{sheets.m_sheetWidth} * sheets.m_sheetHeight * 4 > sheets.m_sheetSize) {
        refusal = "sheets past the end of the font";
        return std::nullopt;
    }
    size_t map = readU32(font, info + 0x1c);
    while (map != 0) {
        const size_t block = map - kBlockHeader;
        if (!magicAt(font, block, "CMAP") || block + 0x14 > font.size()) {
            refusal = "a CMAP link that is not a CMAP block";
            return std::nullopt;
        }
        const uint16_t first = readU16(font, block + 8);
        const uint16_t last = readU16(font, block + 0xa);
        const uint16_t method = readU16(font, block + 0xc);
        const size_t body = block + 0x14;
        if (method == 0) {
            const uint16_t base = readU16(font, body);
            for (uint32_t code = first; code <= last; ++code) {
                sheets.m_codes.emplace_back(code, static_cast<uint16_t>(base + code - first));
            }
        } else if (method == 1) {
            for (uint32_t code = first; code <= last; ++code) {
                const uint16_t glyph = readU16(font, body + (2 * size_t{code - first}));
                if (glyph != 0xffff) {
                    sheets.m_codes.emplace_back(code, glyph);
                }
            }
        } else {
            const uint16_t count = readU16(font, body);
            for (uint16_t index = 0; index < count; ++index) {
                sheets.m_codes.emplace_back(readU16(font, body + 2 + (4 * size_t{index})),
                                            readU16(font, body + 4 + (4 * size_t{index})));
            }
        }
        map = readU32(font, block + 0x10);
    }
    return sheets;
}

std::optional<uint16_t> FontSheets::glyphIndex(char32_t codePoint) const {
    auto found = std::find_if(m_codes.begin(), m_codes.end(), [codePoint](const auto& entry) {
        return entry.first == codePoint;
    });
    if (found == m_codes.end()) {
        return std::nullopt;
    }
    return found->second;
}

FontSheets::Place FontSheets::placeOf(uint16_t glyph) const {
    const uint32_t perSheet = m_cellsPerRow * m_cellsPerColumn;
    const uint32_t inSheet = glyph % perSheet;
    return {.sheet = glyph / perSheet,
            .x = 1 + ((inSheet % m_cellsPerRow) * (m_cellWidth + 1)),
            .y = 1 + ((inSheet / m_cellsPerRow) * (m_cellHeight + 1))};
}

ElementOffset FontSheets::sheetTiling(const SurfaceTiling& tiling) const {
    return tiling({.width = m_sheetWidth,
                   .height = m_sheetHeight,
                   .format = m_gx2Format,
                   .tileMode = kTileMode2dThin1,
                   .swizzle = 0});
}

RgbaImage FontSheets::readCell(uint16_t glyph, const SurfaceTiling& tiling) const {
    RgbaImage cell(m_cellWidth, m_cellHeight);
    const Place place = placeOf(glyph);
    if (place.sheet >= m_sheetCount) {
        return cell;
    }
    const ElementOffset offset = sheetTiling(tiling);
    const size_t sheet = m_sheetData + (size_t{m_sheetSize} * place.sheet);
    for (uint32_t y = 0; y < m_cellHeight; ++y) {
        const uint32_t stored = m_sheetHeight - 1 - (place.y + y);
        for (uint32_t x = 0; x < m_cellWidth; ++x) {
            const size_t at = sheet + offset(place.x + x, stored);
            std::copy_n(m_font.begin() + static_cast<ptrdiff_t>(at), 4, cell.at(x, y));
        }
    }
    return cell;
}

bool FontSheets::writeCell(uint16_t glyph, const RgbaImage& cell, const SurfaceTiling& tiling,
                           std::string& refusal) {
    const Place place = placeOf(glyph);
    if (place.sheet >= m_sheetCount) {
        refusal = "glyph " + std::to_string(glyph) + " is past the last sheet";
        return false;
    }
    if (cell.width != m_cellWidth || cell.height != m_cellHeight) {
        refusal = "a cell drawn at another size";
        return false;
    }
    const ElementOffset offset = sheetTiling(tiling);
    const size_t sheet = m_sheetData + (size_t{m_sheetSize} * place.sheet);
    for (uint32_t y = 0; y < m_cellHeight; ++y) {
        const uint32_t stored = m_sheetHeight - 1 - (place.y + y);
        for (uint32_t x = 0; x < m_cellWidth; ++x) {
            const size_t at = sheet + offset(place.x + x, stored);
            std::copy_n(cell.at(x, y), 4, m_font.begin() + static_cast<ptrdiff_t>(at));
        }
    }
    return true;
}

} // namespace wiiuport::content
