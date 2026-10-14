#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace wiiuport::content {

// Wii U content is big-endian; callers bounds-check before reading.
inline uint16_t readU16(std::span<const uint8_t> bytes, size_t at) {
    return static_cast<uint16_t>((bytes[at] << 8) | bytes[at + 1]);
}

inline uint32_t readU32(std::span<const uint8_t> bytes, size_t at) {
    return (uint32_t{bytes[at]} << 24) | (uint32_t{bytes[at + 1]} << 16) |
           (uint32_t{bytes[at + 2]} << 8) | bytes[at + 3];
}

inline void writeU16(std::span<uint8_t> bytes, size_t at, uint16_t value) {
    bytes[at] = static_cast<uint8_t>(value >> 8);
    bytes[at + 1] = static_cast<uint8_t>(value);
}

inline void writeU32(std::span<uint8_t> bytes, size_t at, uint32_t value) {
    bytes[at] = static_cast<uint8_t>(value >> 24);
    bytes[at + 1] = static_cast<uint8_t>(value >> 16);
    bytes[at + 2] = static_cast<uint8_t>(value >> 8);
    bytes[at + 3] = static_cast<uint8_t>(value);
}

} // namespace wiiuport::content
