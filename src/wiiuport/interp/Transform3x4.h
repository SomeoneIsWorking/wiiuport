#pragma once

#include <array>
#include <cstddef>

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

    // Blends a view transform -- world to camera -- as the camera's pose.
    //
    // Its translation column is not where the camera is: it is the world
    // origin seen from the camera, so a camera turning on the spot sweeps it
    // through an arc. Lerping that column pulls the camera towards the world
    // origin on every in-between frame, by more the further out it stands --
    // tens of units at the coordinates a large world uses. Inverting, blending
    // the pose, and inverting back keeps the camera on its path. Exact at both
    // ends, like blend().
    static Transform3x4 blendView(const Transform3x4& from, const Transform3x4& to, float t);

    // The inverse of a rotation-and-translation: transposed rotation, and the
    // translation taken back through it. Only meaningful for a rigid transform.
    Transform3x4 rigidInverse() const;

    // The angle, in radians, of the rotation that takes `from`'s rotation to
    // `to`'s. What a blend has to sweep through, so the measure of whether
    // two values are one motion or a cut.
    static float rotationAngleBetween(const Transform3x4& from, const Transform3x4& to);

    // Where these twelve floats first occur in a buffer of `width` floats, or
    // kNotFound. Compared as floats, so a zero and a negative zero match.
    static constexpr size_t kNotFound = static_cast<size_t>(-1);
    size_t findIn(const float* buffer, size_t width) const;

    const std::array<float, kFloats>& values() const {
        return m_values;
    }

    bool operator==(const Transform3x4& other) const = default;

  private:
    std::array<float, kFloats> m_values{};
};

} // namespace wiiuport::interp
