#pragma once

#include <cstdint>

namespace wiiuport::title {

// One 32-bit float component of a vertex attribute, decoded in the byte order the fetch declares.
class VertexComponent {
  public:
    // LatteConst::VertexFetchEndianMode, as resolved by GX2 before the draw.
    static constexpr uint8_t kSwapNone = 0;
    static constexpr uint8_t kSwapU16 = 1;
    static constexpr uint8_t kSwapU32 = 2;

    static float read(const uint8_t* bytes, uint8_t endianSwap);
};

} // namespace wiiuport::title
