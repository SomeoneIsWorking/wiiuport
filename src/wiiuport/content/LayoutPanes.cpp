#include "wiiuport/content/LayoutPanes.h"

#include "wiiuport/content/BigEndian.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace wiiuport::content {
namespace {

constexpr size_t kPaneName = 0xc;
constexpr size_t kPaneNameSize = 0x18;
constexpr size_t kPaneWidth = 0x4c;
constexpr size_t kMaterialName = 0x1c;
constexpr size_t kMaterialWhite = 0x20;

} // namespace

std::optional<LayoutPanes> LayoutPanes::parse(std::vector<uint8_t> layout, std::string& refusal) {
    if (layout.size() < 0x14 || std::memcmp(layout.data(), "FLYT", 4) != 0 ||
        readU16(layout, 4) != 0xfeff) {
        refusal = "not a big-endian FLYT layout";
        return std::nullopt;
    }
    size_t at = readU16(layout, 6);
    const uint16_t count = readU16(layout, 0x10);
    for (uint16_t index = 0; index < count; ++index) {
        if (at + 8 > layout.size() || readU32(layout, at + 4) < 8 ||
            at + readU32(layout, at + 4) > layout.size()) {
            refusal = "a section past the end of the layout";
            return std::nullopt;
        }
        at += readU32(layout, at + 4);
    }
    LayoutPanes panes;
    panes.m_bytes = std::move(layout);
    return panes;
}

std::vector<size_t> LayoutPanes::textPanes(std::string_view pane) const {
    std::vector<size_t> found;
    size_t at = readU16(m_bytes, 6);
    const uint16_t count = readU16(m_bytes, 0x10);
    for (uint16_t index = 0; index < count; ++index) {
        const char* name = reinterpret_cast<const char*>(&m_bytes[at + kPaneName]);
        if (std::memcmp(&m_bytes[at], "txt1", 4) == 0 &&
            std::string_view(name, strnlen(name, kPaneNameSize)) == pane) {
            found.push_back(at);
        }
        at += readU32(m_bytes, at + 4);
    }
    return found;
}

std::optional<float> LayoutPanes::width(std::string_view pane) const {
    const std::vector<size_t> found = textPanes(pane);
    if (found.empty()) {
        return std::nullopt;
    }
    return std::bit_cast<float>(readU32(m_bytes, found.front() + kPaneWidth));
}

int LayoutPanes::setWidth(std::string_view pane, float width) {
    const std::vector<size_t> found = textPanes(pane);
    for (size_t at : found) {
        writeU32(m_bytes, at + kPaneWidth, std::bit_cast<uint32_t>(width));
    }
    return static_cast<int>(found.size());
}

int LayoutPanes::setWhite(std::string_view material, Color white) {
    int set = 0;
    size_t at = readU16(m_bytes, 6);
    const uint16_t count = readU16(m_bytes, 0x10);
    for (uint16_t index = 0; index < count; ++index) {
        if (std::memcmp(&m_bytes[at], "mat1", 4) == 0) {
            const uint16_t materials = readU16(m_bytes, at + 8);
            for (uint16_t entry = 0; entry < materials; ++entry) {
                const size_t found = at + readU32(m_bytes, at + 0xc + (4 * size_t{entry}));
                const char* name = reinterpret_cast<const char*>(&m_bytes[found]);
                if (std::string_view(name, strnlen(name, kMaterialName)) == material) {
                    std::copy(white.begin(), white.end(),
                              m_bytes.begin() + static_cast<ptrdiff_t>(found + kMaterialWhite));
                    ++set;
                }
            }
        }
        at += readU32(m_bytes, at + 4);
    }
    return set;
}

} // namespace wiiuport::content
