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

    explicit VertexComponent(uint8_t endianSwap) : m_endianSwap(endianSwap) {
    }

    float read(const uint8_t* bytes) const;
    void write(uint8_t* bytes, float value) const;

  private:
    // Between memory order and host order; each mode is its own inverse.
    uint32_t swap(uint32_t word) const;

    uint8_t m_endianSwap;
};

} // namespace wiiuport::title
