#pragma once

#include "wiiuport/content/RgbaImage.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace wiiuport::glyphs {

// Xbox-layout button glyphs, drawn by this project's code.
class XboxGlyphArt {
  public:
    enum class Shape : uint8_t {
        Face,
        Bumper,
        Trigger,
        Stick,
        Menu,
        View,
        Burst
    };
    enum class Push : uint8_t {
        None,
        Up,
        Down,
        Left,
        Right
    };
    using Color = std::array<uint8_t, 4>;

    struct Glyph {
        Shape shape = Shape::Face;
        std::string_view label;
        Color labelColor{};
        // A stick glyph's arrow, the way it is to be pushed.
        Push push = Push::None;
    };

    static constexpr Color kGreen{107, 190, 46, 255};
    static constexpr Color kRed{226, 56, 44, 255};
    static constexpr Color kBlue{46, 138, 230, 255};
    static constexpr Color kYellow{242, 194, 27, 255};
    static constexpr Color kWhite{240, 240, 240, 255};
    static constexpr Color kBodyTop{72, 72, 76, 255};
    static constexpr Color kBodyBottom{26, 26, 28, 255};

    static content::RgbaImage draw(const Glyph& glyph, uint32_t width, uint32_t height);
};

} // namespace wiiuport::glyphs
