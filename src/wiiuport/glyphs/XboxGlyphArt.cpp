#include "wiiuport/glyphs/XboxGlyphArt.h"

#include "wiiuport/glyphs/VectorCanvas.h"

#include <cmath>
#include <numbers>
#include <vector>

namespace wiiuport::glyphs {
namespace {

using Point = VectorCanvas::Point;

// Art is laid out on a 54-pixel cell and scaled to the cell it is drawn into.
constexpr float kDesign = 54;
constexpr XboxGlyphArt::Color kRim{122, 122, 128, 255};
constexpr XboxGlyphArt::Color kArrow{240, 84, 30, 255};
constexpr XboxGlyphArt::Color kArrowDeep{214, 40, 22, 255};
constexpr int kBurstPoints = 16;

void arrow(VectorCanvas& canvas, float s, Point tail, Point tip) {
    const float dx = tip.x - tail.x;
    const float dy = tip.y - tail.y;
    const float length = std::hypot(dx, dy);
    const Point along{dx / length, dy / length};
    const Point across{-along.y, along.x};
    const float head = 9 * s;
    const Point base{tip.x - (along.x * head), tip.y - (along.y * head)};
    canvas.line(tail, base, 6 * s, kArrowDeep);
    const std::array<Point, 3> triangle{
        tip, Point{base.x + (across.x * 7 * s), base.y + (across.y * 7 * s)},
        Point{base.x - (across.x * 7 * s), base.y - (across.y * 7 * s)}};
    canvas.polygon(triangle, kArrow);
}

void stick(VectorCanvas& canvas, float s, const XboxGlyphArt::Glyph& glyph) {
    Point centre{27 * s, 27 * s};
    float radius = 21 * s;
    const float far = 2 * s;
    const float near = 18 * s;
    switch (glyph.push) {
    case XboxGlyphArt::Push::Up:
        centre = {27 * s, 20 * s};
        break;
    case XboxGlyphArt::Push::Down:
        centre = {27 * s, 34 * s};
        break;
    case XboxGlyphArt::Push::Left:
        centre = {20 * s, 27 * s};
        break;
    case XboxGlyphArt::Push::Right:
        centre = {34 * s, 27 * s};
        break;
    case XboxGlyphArt::Push::None:
        break;
    }
    if (glyph.push != XboxGlyphArt::Push::None) {
        radius = 17 * s;
    }
    canvas.disc(centre, radius, XboxGlyphArt::kBodyTop, XboxGlyphArt::kBodyBottom);
    canvas.ring(centre, radius - (0.75F * s), 1.5F * s, kRim);
    canvas.ring(centre, radius * 0.7F, 1.5F * s, kRim);
    if (!glyph.label.empty()) {
        canvas.text(glyph.label, centre, 12 * s, 2.2F * s, glyph.labelColor);
    }
    const float edge = kDesign * s;
    switch (glyph.push) {
    case XboxGlyphArt::Push::Up:
        arrow(canvas, s, {27 * s, edge - far}, {27 * s, edge - near});
        break;
    case XboxGlyphArt::Push::Down:
        arrow(canvas, s, {27 * s, far}, {27 * s, near});
        break;
    case XboxGlyphArt::Push::Left:
        arrow(canvas, s, {edge - far, 27 * s}, {edge - near, 27 * s});
        break;
    case XboxGlyphArt::Push::Right:
        arrow(canvas, s, {far, 27 * s}, {near, 27 * s});
        break;
    case XboxGlyphArt::Push::None:
        break;
    }
}

void burst(VectorCanvas& canvas, float s, const XboxGlyphArt::Glyph& glyph) {
    std::vector<Point> star;
    for (int i = 0; i < kBurstPoints * 2; ++i) {
        const float radius = (i % 2 == 0 ? 26.0F : 20.0F) * s;
        const float angle = std::numbers::pi_v<float> * static_cast<float>(i) / kBurstPoints;
        star.push_back(
            {(27 * s) + (radius * std::cos(angle)), (27 * s) + (radius * std::sin(angle))});
    }
    canvas.polygon(star, XboxGlyphArt::kBodyBottom);
    canvas.disc({27 * s, 27 * s}, 18 * s, glyph.labelColor, glyph.labelColor);
    canvas.text(glyph.label, {27 * s, 27 * s}, 20 * s, 4 * s, XboxGlyphArt::kWhite);
}

} // namespace

content::RgbaImage XboxGlyphArt::draw(const Glyph& glyph, uint32_t width, uint32_t height) {
    content::RgbaImage image(width, height);
    VectorCanvas canvas(image);
    const float s = static_cast<float>(std::min(width, height)) / kDesign;
    const Point centre{27 * s, 27 * s};
    switch (glyph.shape) {
    case Shape::Face:
        canvas.disc(centre, 23 * s, kBodyTop, kBodyBottom);
        canvas.ring(centre, 22.25F * s, 1.5F * s, kRim);
        canvas.text(glyph.label, centre, 24 * s, 4.5F * s, glyph.labelColor);
        break;
    case Shape::Bumper:
        canvas.roundedRect({3 * s, 14 * s}, {51 * s, 40 * s}, 9 * s, kBodyTop, kBodyBottom);
        canvas.text(glyph.label, centre, 15 * s, 3.2F * s, glyph.labelColor);
        break;
    case Shape::Trigger:
        canvas.roundedRect({7 * s, 4 * s}, {47 * s, 50 * s}, 12 * s, kBodyTop, kBodyBottom);
        canvas.text(glyph.label, centre, 15 * s, 3.2F * s, glyph.labelColor);
        break;
    case Shape::Stick:
        stick(canvas, s, glyph);
        break;
    case Shape::Menu:
        canvas.disc(centre, 21 * s, kBodyTop, kBodyBottom);
        for (float y : {20.0F, 27.0F, 34.0F}) {
            canvas.line({19 * s, y * s}, {35 * s, y * s}, 3 * s, glyph.labelColor);
        }
        break;
    case Shape::View:
        canvas.disc(centre, 21 * s, kBodyTop, kBodyBottom);
        canvas.line({17 * s, 17 * s}, {30 * s, 17 * s}, 2.5F * s, glyph.labelColor);
        canvas.line({17 * s, 17 * s}, {17 * s, 30 * s}, 2.5F * s, glyph.labelColor);
        canvas.roundedRect({23 * s, 23 * s}, {38 * s, 37 * s}, 2 * s, glyph.labelColor,
                           glyph.labelColor);
        break;
    case Shape::Burst:
        burst(canvas, s, glyph);
        break;
    }
    return image;
}

} // namespace wiiuport::glyphs
