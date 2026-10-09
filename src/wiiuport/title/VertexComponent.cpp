#include "wiiuport/title/VertexComponent.h"

#include <cstring>

namespace wiiuport::title {

uint32_t VertexComponent::swap(uint32_t word) const {
    if (m_endianSwap == kSwapU16 || m_endianSwap == kSwapU32) {
        word = ((word & 0x00ff00ffu) << 8) | ((word >> 8) & 0x00ff00ffu);
    }
    if (m_endianSwap == kSwapU32) {
        word = (word << 16) | (word >> 16);
    }
    return word;
}

float VertexComponent::read(const uint8_t* bytes) const {
    uint32_t word = 0;
    std::memcpy(&word, bytes, sizeof(word));
    word = swap(word);
    float value = 0.0f;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

void VertexComponent::write(uint8_t* bytes, float value) const {
    uint32_t word = 0;
    std::memcpy(&word, &value, sizeof(word));
    word = swap(word);
    std::memcpy(bytes, &word, sizeof(word));
}

} // namespace wiiuport::title
