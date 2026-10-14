#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wiiuport::content {

// Straight-alpha RGBA8, rows top to bottom.
struct RgbaImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;

    RgbaImage() = default;

    RgbaImage(uint32_t w, uint32_t h) : width(w), height(h), pixels(size_t{w} * h * 4, 0) {
    }

    uint8_t* at(uint32_t x, uint32_t y) {
        return &pixels[((size_t{y} * width) + x) * 4];
    }

    const uint8_t* at(uint32_t x, uint32_t y) const {
        return &pixels[((size_t{y} * width) + x) * 4];
    }
};

} // namespace wiiuport::content
