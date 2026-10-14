#include "wiiuport/glyphs/VectorCanvas.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wiiuport::glyphs {
namespace {

using Point = VectorCanvas::Point;

constexpr float kLetterWidth = 0.68F;
constexpr float kLetterGap = 0.3F;
constexpr int kArcSteps = 24;

float clamp01(float value) {
    return std::clamp(value, 0.0F, 1.0F);
}

float segmentDistance(Point p, Point a, Point b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float lengthSquared = (dx * dx) + (dy * dy);
    const float t =
        lengthSquared == 0 ? 0 : clamp01((((p.x - a.x) * dx) + ((p.y - a.y) * dy)) / lengthSquared);
    return std::hypot(p.x - (a.x + (t * dx)), p.y - (a.y + (t * dy)));
}

// From ends[0] at t 0 to ends[1] at t 1.
VectorCanvas::Color mix(const std::array<VectorCanvas::Color, 2>& ends, float t) {
    VectorCanvas::Color out{};
    for (size_t i = 0; i < 4; ++i) {
        const auto from = static_cast<float>(ends[0][i]);
        out[i] = static_cast<uint8_t>(
            std::lround(from + ((static_cast<float>(ends[1][i]) - from) * clamp01(t))));
    }
    return out;
}

} // namespace

void VectorCanvas::blend(uint32_t x, uint32_t y, Color color, float coverage) {
    if (x >= m_image.width || y >= m_image.height || coverage <= 0) {
        return;
    }
    uint8_t* pixel = m_image.at(x, y);
    const float sourceAlpha = (static_cast<float>(color[3]) / 255.0F) * clamp01(coverage);
    const float destinationAlpha = static_cast<float>(pixel[3]) / 255.0F;
    const float alpha = sourceAlpha + (destinationAlpha * (1 - sourceAlpha));
    for (size_t i = 0; i < 3; ++i) {
        const float source = static_cast<float>(color[i]) * sourceAlpha;
        const float destination =
            static_cast<float>(pixel[i]) * destinationAlpha * (1 - sourceAlpha);
        const float value = alpha == 0 ? 0 : (source + destination) / alpha;
        pixel[i] = static_cast<uint8_t>(std::lround(value));
    }
    pixel[3] = static_cast<uint8_t>(std::lround(alpha * 255));
}

template <typename Coverage>
void VectorCanvas::fill(Point min, Point max, Color top, Color bottom, Coverage coverage) {
    const auto x0 = static_cast<uint32_t>(std::max(0.0F, std::floor(min.x - 1)));
    const auto y0 = static_cast<uint32_t>(std::max(0.0F, std::floor(min.y - 1)));
    const auto x1 =
        static_cast<uint32_t>(std::min(static_cast<float>(m_image.width), std::ceil(max.x + 1)));
    const auto y1 =
        static_cast<uint32_t>(std::min(static_cast<float>(m_image.height), std::ceil(max.y + 1)));
    const float span = std::max(1.0F, max.y - min.y);
    for (uint32_t y = y0; y < y1; ++y) {
        const Color color = mix({top, bottom}, (static_cast<float>(y) + 0.5F - min.y) / span);
        for (uint32_t x = x0; x < x1; ++x) {
            // Signed distance outside the shape; half a pixel of ramp either side.
            const float outside =
                coverage(Point{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F});
            blend(x, y, color, 0.5F - outside);
        }
    }
}

void VectorCanvas::disc(Point centre, float radius, Color top, Color bottom) {
    fill({centre.x - radius, centre.y - radius}, {centre.x + radius, centre.y + radius}, top,
         bottom, [&](Point p) {
             return std::hypot(p.x - centre.x, p.y - centre.y) - radius;
         });
}

void VectorCanvas::ring(Point centre, float radius, float width, Color color) {
    const float outer = radius + (width / 2);
    fill({centre.x - outer, centre.y - outer}, {centre.x + outer, centre.y + outer}, color, color,
         [&](Point p) {
             return std::abs(std::hypot(p.x - centre.x, p.y - centre.y) - radius) - (width / 2);
         });
}

void VectorCanvas::roundedRect(Point min, Point max, float radius, Color top, Color bottom) {
    const Point centre{(min.x + max.x) / 2, (min.y + max.y) / 2};
    const Point half{((max.x - min.x) / 2) - radius, ((max.y - min.y) / 2) - radius};
    fill(min, max, top, bottom, [&](Point p) {
        const float dx = std::max(std::abs(p.x - centre.x) - half.x, 0.0F);
        const float dy = std::max(std::abs(p.y - centre.y) - half.y, 0.0F);
        return std::hypot(dx, dy) - radius;
    });
}

void VectorCanvas::polygon(std::span<const Point> points, Color color) {
    Point min{points[0]};
    Point max{points[0]};
    for (const Point& p : points) {
        min = {std::min(min.x, p.x), std::min(min.y, p.y)};
        max = {std::max(max.x, p.x), std::max(max.y, p.y)};
    }
    fill(min, max, color, color, [&](Point p) {
        float distance = 1e9F;
        bool inside = false;
        for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
            const Point a = points[i];
            const Point b = points[j];
            distance = std::min(distance, segmentDistance(p, a, b));
            if (((a.y > p.y) != (b.y > p.y)) &&
                (p.x < ((b.x - a.x) * (p.y - a.y) / (b.y - a.y)) + a.x)) {
                inside = !inside;
            }
        }
        return inside ? -distance : distance;
    });
}

void VectorCanvas::line(Point from, Point to, float width, Color color) {
    const float half = width / 2;
    fill({std::min(from.x, to.x) - half, std::min(from.y, to.y) - half},
         {std::max(from.x, to.x) + half, std::max(from.y, to.y) + half}, color, color,
         [&](Point p) {
             return segmentDistance(p, from, to) - half;
         });
}

void VectorCanvas::arc(const Arc& arc, float width, Color color) {
    const Point centre = arc.centre;
    const float radius = arc.radius;
    const float fromDegrees = arc.fromDegrees;
    const float toDegrees = arc.toDegrees;
    std::vector<Point> path;
    for (int step = 0; step <= kArcSteps; ++step) {
        const float degrees = fromDegrees + ((toDegrees - fromDegrees) * static_cast<float>(step) /
                                             static_cast<float>(kArcSteps));
        const float radians = degrees * std::numbers::pi_v<float> / 180.0F;
        path.push_back(
            {centre.x + (radius * std::cos(radians)), centre.y + (radius * std::sin(radians))});
    }
    const float half = width / 2;
    fill({centre.x - radius - half, centre.y - radius - half},
         {centre.x + radius + half, centre.y + radius + half}, color, color, [&](Point p) {
             float distance = 1e9F;
             for (size_t i = 1; i < path.size(); ++i) {
                 distance = std::min(distance, segmentDistance(p, path[i - 1], path[i]));
             }
             return distance - half;
         });
}

float VectorCanvas::textWidth(std::string_view letters, float height, float stroke) {
    if (letters.empty()) {
        return 0;
    }
    const auto count = static_cast<float>(letters.size());
    return (count * kLetterWidth * height) + ((count - 1) * kLetterGap * height) + stroke;
}

void VectorCanvas::text(std::string_view letters, Point centre, float height, float stroke,
                        Color color) {
    const float width = textWidth(letters, height, stroke) - stroke;
    Point origin{centre.x - (width / 2), centre.y - (height / 2)};
    for (char letter : letters) {
        this->letter(letter, origin, height, stroke, color);
        origin.x += (kLetterWidth + kLetterGap) * height;
    }
}

// Strokes in a box kLetterWidth wide and 1 high, scaled by `height` from `origin`.
void VectorCanvas::letter(char letter, Point origin, float height, float stroke, Color color) {
    const float w = kLetterWidth;
    auto at = [&](float x, float y) {
        return Point{origin.x + (x * height), origin.y + (y * height)};
    };
    auto segment = [&](float x0, float y0, float x1, float y1) {
        line(at(x0, y0), at(x1, y1), stroke, color);
    };
    auto bow = [&](float cx, float cy, float r, float from, float to) {
        arc({.centre = at(cx, cy), .radius = r * height, .fromDegrees = from, .toDegrees = to},
            stroke, color);
    };
    switch (letter) {
    case 'A':
        segment(0, 1, w / 2, 0);
        segment(w / 2, 0, w, 1);
        segment(w * 0.2F, 0.66F, w * 0.8F, 0.66F);
        break;
    case 'B':
        segment(0, 0, 0, 1);
        segment(0, 0, w * 0.55F, 0);
        segment(0, 0.48F, w * 0.6F, 0.48F);
        segment(0, 1, w * 0.6F, 1);
        bow(w * 0.55F, 0.24F, 0.24F, -90, 90);
        bow(w * 0.6F, 0.74F, 0.26F, -90, 90);
        break;
    case 'X':
        segment(0, 0, w, 1);
        segment(w, 0, 0, 1);
        break;
    case 'Y':
        segment(0, 0, w / 2, 0.5F);
        segment(w, 0, w / 2, 0.5F);
        segment(w / 2, 0.5F, w / 2, 1);
        break;
    case 'L':
        segment(0, 0, 0, 1);
        segment(0, 1, w * 0.85F, 1);
        break;
    case 'R':
        segment(0, 0, 0, 1);
        segment(0, 0, w * 0.55F, 0);
        segment(0, 0.52F, w * 0.55F, 0.52F);
        bow(w * 0.55F, 0.26F, 0.26F, -90, 90);
        segment(w * 0.45F, 0.52F, w, 1);
        break;
    case 'T':
        segment(0, 0, w, 0);
        segment(w / 2, 0, w / 2, 1);
        break;
    case 'S':
        bow(w / 2, 0.255F, 0.255F, -35, -270);
        bow(w / 2, 0.745F, 0.255F, -90, 145);
        break;
    default:
        break;
    }
}

} // namespace wiiuport::glyphs
