#pragma once

#include <cmath>

namespace wiiuport::interp {

// A value that can be blended: zero or a finite normal float. Small integers
// stored in a float's bits read as denormals, and blending two of them makes
// an integer neither frame wrote.
inline bool isNumber(float value) {
    return value == 0.0f || std::isnormal(value);
}

} // namespace wiiuport::interp
