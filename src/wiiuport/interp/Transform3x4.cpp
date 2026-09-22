#include "wiiuport/interp/Transform3x4.h"

#include <algorithm>
#include <cmath>

namespace wiiuport::interp {
namespace {

// Above this dot product the arc is shorter than float precision can
// resolve an angle across, so slerp's division by sin(theta) loses more
// than the straight lerp it falls back to.
inline constexpr float kNearlyParallel = 0.9995f;

struct Quaternion {
    float w{1.0f};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

// Shepperd's method: pick the branch whose divisor is largest. Taking the
// trace branch unconditionally loses precision, and divides by nearly zero for
// a rotation near 180 degrees.
Quaternion toQuaternion(const std::array<float, Transform3x4::kFloats>& m) {
    float m00 = m[0];
    float m01 = m[1];
    float m02 = m[2];
    float m10 = m[4];
    float m11 = m[5];
    float m12 = m[6];
    float m20 = m[8];
    float m21 = m[9];
    float m22 = m[10];
    float trace = m00 + m11 + m22;
    Quaternion q;
    if (trace > 0.0f) {
        float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

void writeRotation(const Quaternion& q, std::array<float, Transform3x4::kFloats>& m) {
    float xx = q.x * q.x;
    float yy = q.y * q.y;
    float zz = q.z * q.z;
    m[0] = 1.0f - 2.0f * (yy + zz);
    m[1] = 2.0f * (q.x * q.y - q.z * q.w);
    m[2] = 2.0f * (q.x * q.z + q.y * q.w);
    m[4] = 2.0f * (q.x * q.y + q.z * q.w);
    m[5] = 1.0f - 2.0f * (xx + zz);
    m[6] = 2.0f * (q.y * q.z - q.x * q.w);
    m[8] = 2.0f * (q.x * q.z - q.y * q.w);
    m[9] = 2.0f * (q.y * q.z + q.x * q.w);
    m[10] = 1.0f - 2.0f * (xx + yy);
}

Quaternion slerp(Quaternion a, const Quaternion& b, float t) {
    float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    // q and -q are the same rotation. Without this the blend takes the long
    // way round whenever the sign flips, which shows up as the view spinning
    // almost all the way about on a single frame.
    if (dot < 0.0f) {
        a = Quaternion{-a.w, -a.x, -a.y, -a.z};
        dot = -dot;
    }

    Quaternion result;
    if (dot > kNearlyParallel) {
        // Too close for the angle to be recovered accurately; a straight lerp
        // and a normalise is within float precision of the arc here.
        result = Quaternion{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                            a.z + (b.z - a.z) * t};
    } else {
        float theta = std::acos(dot);
        float sinTheta = std::sin(theta);
        float weightA = std::sin((1.0f - t) * theta) / sinTheta;
        float weightB = std::sin(t * theta) / sinTheta;
        result = Quaternion{a.w * weightA + b.w * weightB, a.x * weightA + b.x * weightB,
                            a.y * weightA + b.y * weightB, a.z * weightA + b.z * weightB};
    }
    float length = std::sqrt(result.w * result.w + result.x * result.x + result.y * result.y +
                             result.z * result.z);
    if (length > 0.0f) {
        result =
            Quaternion{result.w / length, result.x / length, result.y / length, result.z / length};
    }
    return result;
}

} // namespace

Transform3x4 Transform3x4::fromRowMajor(const float* values) {
    Transform3x4 transform;
    std::copy(values, values + kFloats, transform.m_values.begin());
    return transform;
}

void Transform3x4::writeRowMajor(float* values) const {
    std::copy(m_values.begin(), m_values.end(), values);
}

Vec3 Transform3x4::translation() const {
    return Vec3{m_values[3], m_values[7], m_values[11]};
}

float Transform3x4::rotationError() const {
    std::array<Vec3, 3> rows{Vec3{m_values[0], m_values[1], m_values[2]},
                             Vec3{m_values[4], m_values[5], m_values[6]},
                             Vec3{m_values[8], m_values[9], m_values[10]}};
    auto dot = [](const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    float error = 0.0f;
    for (size_t i = 0; i < rows.size(); i++) {
        error = std::max(error, std::abs(std::sqrt(dot(rows[i], rows[i])) - 1.0f));
        for (size_t j = i + 1; j < rows.size(); j++) {
            error = std::max(error, std::abs(dot(rows[i], rows[j])));
        }
    }
    return error;
}

Transform3x4 Transform3x4::blend(const Transform3x4& from, const Transform3x4& to, float t) {
    float clamped = std::clamp(t, 0.0f, 1.0f);
    // Ends that are one transform blend to it exactly. The general path only
    // approaches it, through a quaternion and back, and a held-still scene's
    // in-between frame has to be the title's bit for bit.
    if (clamped <= 0.0f || from == to) {
        return from;
    }
    if (clamped >= 1.0f) {
        return to;
    }
    Transform3x4 result;
    writeRotation(slerp(toQuaternion(from.m_values), toQuaternion(to.m_values), clamped),
                  result.m_values);
    Vec3 a = from.translation();
    Vec3 b = to.translation();
    result.m_values[3] = a.x + (b.x - a.x) * clamped;
    result.m_values[7] = a.y + (b.y - a.y) * clamped;
    result.m_values[11] = a.z + (b.z - a.z) * clamped;
    return result;
}

Transform3x4 Transform3x4::rigidInverse() const {
    const auto& m = m_values;
    Transform3x4 inverse;
    auto& r = inverse.m_values;
    // Rows of the inverse rotation are the columns of this one.
    r[0] = m[0];
    r[1] = m[4];
    r[2] = m[8];
    r[4] = m[1];
    r[5] = m[5];
    r[6] = m[9];
    r[8] = m[2];
    r[9] = m[6];
    r[10] = m[10];
    r[3] = -(r[0] * m[3] + r[1] * m[7] + r[2] * m[11]);
    r[7] = -(r[4] * m[3] + r[5] * m[7] + r[6] * m[11]);
    r[11] = -(r[8] * m[3] + r[9] * m[7] + r[10] * m[11]);
    return inverse;
}

Transform3x4 Transform3x4::blendView(const Transform3x4& from, const Transform3x4& to, float t) {
    float clamped = std::clamp(t, 0.0f, 1.0f);
    // As in blend(): the two rigid inverses would round a held view.
    if (clamped <= 0.0f || from == to) {
        return from;
    }
    if (clamped >= 1.0f) {
        return to;
    }
    return blend(from.rigidInverse(), to.rigidInverse(), clamped).rigidInverse();
}

float Transform3x4::rotationAngleBetween(const Transform3x4& from, const Transform3x4& to) {
    Quaternion a = toQuaternion(from.m_values);
    Quaternion b = toQuaternion(to.m_values);
    float dot = std::abs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
    return 2.0f * std::acos(std::min(dot, 1.0f));
}

size_t Transform3x4::findIn(const float* buffer, size_t width) const {
    if (width < static_cast<size_t>(kFloats)) {
        return kNotFound;
    }
    for (size_t offset = 0; offset + kFloats <= width; ++offset) {
        if (std::equal(m_values.begin(), m_values.end(), buffer + offset)) {
            return offset;
        }
    }
    return kNotFound;
}

} // namespace wiiuport::interp
