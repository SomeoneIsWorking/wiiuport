#pragma once

#include "wiiuport/content/RgbaImage.h"
#include "wiiuport/content/SurfaceTiling.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wiiuport::content {

// The glyph sheets of a Wii U font (FFNT, BFFNT): reads and replaces one glyph's cell.
//
// Sheets are GX2 surfaces stored bottom row first; cells sit one pixel apart from a one-pixel
// border. Only RGBA8 sheets (picture fonts) are handled.
class FontSheets {
  public:
    // GX2 surface formats for the sheet formats handled.
    static constexpr uint32_t kGx2Rgba8 = 0x1a;
    static constexpr uint32_t kGx2Rgba8Srgb = 0x41a;
    static constexpr uint32_t kTileMode2dThin1 = 4;

    static std::optional<FontSheets> parse(std::span<uint8_t> font, std::string& refusal);

    std::optional<uint16_t> glyphIndex(char32_t codePoint) const;

    uint32_t cellWidth() const {
        return m_cellWidth;
    }

    uint32_t cellHeight() const {
        return m_cellHeight;
    }

    RgbaImage readCell(uint16_t glyph, const SurfaceTiling& tiling) const;
    // `cell` is cellWidth by cellHeight.
    bool writeCell(uint16_t glyph, const RgbaImage& cell, const SurfaceTiling& tiling,
                   std::string& refusal);

  private:
    struct Place {
        uint32_t sheet = 0;
        uint32_t x = 0;
        uint32_t y = 0;
    };

    Place placeOf(uint16_t glyph) const;
    ElementOffset sheetTiling(const SurfaceTiling& tiling) const;

    std::span<uint8_t> m_font;
    uint32_t m_cellWidth = 0;
    uint32_t m_cellHeight = 0;
    uint32_t m_sheetCount = 0;
    uint32_t m_sheetSize = 0;
    uint32_t m_gx2Format = 0;
    uint32_t m_cellsPerRow = 0;
    uint32_t m_cellsPerColumn = 0;
    uint32_t m_sheetWidth = 0;
    uint32_t m_sheetHeight = 0;
    uint32_t m_sheetData = 0;
    std::vector<std::pair<char32_t, uint16_t>> m_codes;
};

} // namespace wiiuport::content
