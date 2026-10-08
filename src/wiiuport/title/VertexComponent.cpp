#include "wiiuport/title/VertexComponent.h"

#include <cstring>

namespace wiiuport::title {

float VertexComponent::read(const uint8_t* bytes, uint8_t endianSwap) {
    uint32_t word = 0;
    std::memcpy(&word, bytes, sizeof(word));
    if (endianSwap == kSwapU16 || endianSwap == kSwapU32) {
        word = ((word & 0x00ff00ffu) << 8) | ((word >> 8) & 0x00ff00ffu);
    }
    if (endianSwap == kSwapU32) {
        word = (word << 16) | (word >> 16);
    }
    float value = 0.0f;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

} // namespace wiiuport::title
