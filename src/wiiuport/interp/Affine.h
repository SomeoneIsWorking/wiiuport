#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace wiiuport::interp {

// A 3x4 affine transform, row-major with the translation in the fourth column (GX's Mtx).
struct Affine {
    std::array<double, 12> m{};

    double at(size_t row, size_t column) const {
        return m.at((row * 4) + column);
    }

    double& at(size_t row, size_t column) {
        return m.at((row * 4) + column);
    }
};

inline Affine operator*(const Affine& l, const Affine& r) {
    Affine out;
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            double sum = column == 3 ? l.at(row, 3) : 0.0;
            for (size_t k = 0; k < 3; ++k) {
                sum += l.at(row, k) * r.at(k, column);
            }
            out.at(row, column) = sum;
        }
    }
    return out;
}

// The inverse, or nothing when the 3x3 is singular.
inline std::optional<Affine> inverse(const Affine& a) {
    double c00 = (a.at(1, 1) * a.at(2, 2)) - (a.at(1, 2) * a.at(2, 1));
    double c01 = (a.at(1, 2) * a.at(2, 0)) - (a.at(1, 0) * a.at(2, 2));
    double c02 = (a.at(1, 0) * a.at(2, 1)) - (a.at(1, 1) * a.at(2, 0));
    double determinant = (a.at(0, 0) * c00) + (a.at(0, 1) * c01) + (a.at(0, 2) * c02);
    if (!std::isnormal(determinant)) {
        return std::nullopt;
    }
    double scale = 1.0 / determinant;
    Affine out;
    out.at(0, 0) = c00 * scale;
    out.at(1, 0) = c01 * scale;
    out.at(2, 0) = c02 * scale;
    out.at(0, 1) = ((a.at(0, 2) * a.at(2, 1)) - (a.at(0, 1) * a.at(2, 2))) * scale;
    out.at(1, 1) = ((a.at(0, 0) * a.at(2, 2)) - (a.at(0, 2) * a.at(2, 0))) * scale;
    out.at(2, 1) = ((a.at(0, 1) * a.at(2, 0)) - (a.at(0, 0) * a.at(2, 1))) * scale;
    out.at(0, 2) = ((a.at(0, 1) * a.at(1, 2)) - (a.at(0, 2) * a.at(1, 1))) * scale;
    out.at(1, 2) = ((a.at(0, 2) * a.at(1, 0)) - (a.at(0, 0) * a.at(1, 2))) * scale;
    out.at(2, 2) = ((a.at(0, 0) * a.at(1, 1)) - (a.at(0, 1) * a.at(1, 0))) * scale;
    for (size_t row = 0; row < 3; ++row) {
        out.at(row, 3) = -((out.at(row, 0) * a.at(0, 3)) + (out.at(row, 1) * a.at(1, 3)) +
                           (out.at(row, 2) * a.at(2, 3)));
    }
    return out;
}

// Element-wise midpoint: for a tick's worth of motion, close to the rigid midpoint.
inline Affine midpoint(const Affine& from, const Affine& to) {
    Affine out;
    for (size_t i = 0; i < out.m.size(); ++i) {
        out.m.at(i) = from.m.at(i) + (0.5 * (to.m.at(i) - from.m.at(i)));
    }
    return out;
}

} // namespace wiiuport::interp
