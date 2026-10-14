#pragma once

#include <cstdint>
#include <functional>

namespace wiiuport::content {

// A GX2 surface's shape, as a texture header gives it.
struct SurfaceShape {
    uint32_t width = 0;
    uint32_t height = 0;
    // GX2 surface format (GX2SurfaceFormat), which sets the bits per element.
    uint32_t format = 0;
    uint32_t tileMode = 0;
    uint32_t swizzle = 0;
};

// Byte offset of element (x, y) in the surface's tiled data. The host builds it from the fork's
// address library, which is the one implementation of GX2 tiling.
using ElementOffset = std::function<uint32_t(uint32_t x, uint32_t y)>;
using SurfaceTiling = std::function<ElementOffset(const SurfaceShape& shape)>;

} // namespace wiiuport::content
