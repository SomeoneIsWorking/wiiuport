#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace wiiuport::interp {

// A value that can be blended: zero or a finite normal float. Small integers
// stored in a float's bits read as denormals, and blending two of them makes
// an integer neither frame wrote.
inline bool isNumber(float value) {
    return value == 0.0f || std::isnormal(value);
}

// The same float bit for bit. Values the title writes are compared this way,
// not by value: -0 and 0 are different writes, and a NaN is equal to itself.
inline bool sameBits(float l, float r) {
    return std::bit_cast<uint32_t>(l) == std::bit_cast<uint32_t>(r);
}

inline bool sameBits(std::span<const float> l, std::span<const float> r) {
    return l.size() == r.size() && std::memcmp(l.data(), r.data(), l.size_bytes()) == 0;
}

} // namespace wiiuport::interp
