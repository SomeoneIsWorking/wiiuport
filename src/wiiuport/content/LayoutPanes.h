#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wiiuport::content {

// A Wii U layout (FLYT, BFLYT): the boxes of its text panes and its materials' colours.
class LayoutPanes {
  public:
    using Color = std::array<uint8_t, 4>;

    static std::optional<LayoutPanes> parse(std::vector<uint8_t> layout, std::string& refusal);

    const std::vector<uint8_t>& bytes() const {
        return m_bytes;
    }

    // The first text pane of that name.
    std::optional<float> width(std::string_view pane) const;
    // Every text pane of that name; returns how many were set.
    int setWidth(std::string_view pane, float width);
    // The colour a material maps white to: a text's colour. Returns how many were set.
    int setWhite(std::string_view material, Color white);

  private:
    std::vector<size_t> textPanes(std::string_view pane) const;

    std::vector<uint8_t> m_bytes;
};

} // namespace wiiuport::content
