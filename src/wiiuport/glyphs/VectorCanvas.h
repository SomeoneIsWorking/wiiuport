#pragma once

#include "wiiuport/content/RgbaImage.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace wiiuport::glyphs {

// Antialiased shapes composited over an RGBA image, measured in pixels.
class VectorCanvas {
  public:
    using Color = std::array<uint8_t, 4>;

    struct Point {
        float x = 0;
        float y = 0;
    };

    explicit VectorCanvas(content::RgbaImage& image) : m_image(image) {
    }

    void disc(Point centre, float radius, Color top, Color bottom);
    void ring(Point centre, float radius, float width, Color color);
    void roundedRect(Point min, Point max, float radius, Color top, Color bottom);
    void polygon(std::span<const Point> points, Color color);
    void line(Point from, Point to, float width, Color color);

    // Angles in degrees, clockwise from +x on a y-down image.
    struct Arc {
        Point centre;
        float radius = 0;
        float fromDegrees = 0;
        float toDegrees = 0;
    };

    void arc(const Arc& arc, float width, Color color);
    // Capital letters and digits from straight and round strokes; `height` is the cap height.
    void text(std::string_view letters, Point centre, float height, float stroke, Color color);
    static float textWidth(std::string_view letters, float height, float stroke);

  private:
    template <typename Coverage>
    void fill(Point min, Point max, Color top, Color bottom, Coverage coverage);
    void blend(uint32_t x, uint32_t y, Color color, float coverage);
    void letter(char letter, Point origin, float height, float stroke, Color color);

    content::RgbaImage& m_image;
};

} // namespace wiiuport::glyphs
