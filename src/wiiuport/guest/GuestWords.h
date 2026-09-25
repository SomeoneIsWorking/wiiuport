#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wiiuport::guest {

// The guest's big-endian word at `offset` bytes into `bytes`.
inline uint32_t guestWord(const void* bytes, size_t offset) {
    uint32_t word = 0;
    std::memcpy(&word, static_cast<const std::byte*>(bytes) + offset, sizeof(word));
    if constexpr (std::endian::native == std::endian::little) {
        word =
            (word >> 24) | ((word >> 8) & 0x0000ff00u) | ((word << 8) & 0x00ff0000u) | (word << 24);
    }
    return word;
}

inline float guestFloat(const void* bytes, size_t offset) {
    return std::bit_cast<float>(guestWord(bytes, offset));
}

} // namespace wiiuport::guest
