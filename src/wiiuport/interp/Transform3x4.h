#pragma once

#include <array>

namespace wiiuport::interp {

struct Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

// A rigid transform as the title stores it: three rows of four, a 3x3 rotation
// with translation in the fourth column.
//
// Blending two of these is the whole point of an interpolated frame, and the
// rotation cannot be blended element by element. Averaging two rotation
// matrices does not give a rotation: the result is shortened towards the
// centre, so a turning camera would also shrink the world slightly on every
// in-between frame. The rotation is interpolated as a rotation and the
// translation linearly.
class Transform3x4 {
  public:
    static constexpr int kFloats = 12;

    Transform3x4() = default;
    static Transform3x4 fromRowMajor(const float* values);

    void writeRowMajor(float* values) const;

    Vec3 translation() const;

    // How far the 3x3 part is from orthonormal: the largest deviation of any
    // row norm from one, or of any pair of rows from perpendicular. Returned
    // rather than thresholded so a caller can see a near miss instead of
    // having it silently accepted or dropped.
    float rotationError() const;

    // Interpolated at `t`, clamped to [0, 1]. Exact at both ends: the values
    // at t=0 and t=1 are the inputs unchanged, so a blend that never fires
    // cannot be mistaken for one that did.
    static Transform3x4 blend(const Transform3x4& from, const Transform3x4& to, float t);

    const std::array<float, kFloats>& values() const {
        return m_values;
    }

    bool operator==(const Transform3x4& other) const = default;

  private:
    std::array<float, kFloats> m_values{};
};

} // namespace wiiuport::interp
