#include "wiiuport/title/TransformShape.h"

#include <algorithm>
#include <cmath>

namespace wiiuport::title {

bool TransformShape::isRigid(const float* words) {
    for (size_t row = 0; row < 3; row++) {
        const float x = words[row * 3 + 0];
        const float y = words[row * 3 + 1];
        const float z = words[row * 3 + 2];
        if (std::fabs(std::sqrt(x * x + y * y + z * z) - 1.0f) > kUnitTolerance) {
            return false;
        }
    }
    for (size_t first = 0; first < 3; first++) {
        for (size_t second = first + 1; second < 3; second++) {
            float dot = 0.0f;
            for (size_t column = 0; column < 3; column++) {
                dot += words[first * 3 + column] * words[second * 3 + column];
            }
            if (std::fabs(dot) > kPerpendicularTolerance) {
                return false;
            }
        }
    }
    return true;
}

bool TransformShape::isAffine(const float* words) {
    return classify(words) == Affine::Yes;
}

TransformShape::Affine TransformShape::classify(const float* words) {
    for (size_t word = 0; word < kWords; word++) {
        if (!std::isfinite(words[word])) {
            return Affine::NotFinite;
        }
    }
    if (determinantOf(words) <= kDeterminantFloor) {
        return Affine::Singular;
    }
    if (scaleOf(words) > kScaleCeiling) {
        return Affine::TooLarge;
    }
    return Affine::Yes;
}

double TransformShape::determinantOf(const float* words) {
    // Doubled rather than floated: a determinant of 1e-15 from denormal rows is a degenerate
    // triple, and the comparison is against 1e-6, so float precision is not the binding
    // constraint here -- but the products of three guest floats are computed in double so that
    // a large-but-finite matrix does not overflow on the way.
    const double a = words[0];
    const double b = words[1];
    const double c = words[2];
    const double d = words[3];
    const double e = words[4];
    const double f = words[5];
    const double g = words[6];
    const double h = words[7];
    const double i = words[8];
    return std::fabs(a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g));
}

float TransformShape::scaleOf(const float* words) {
    float worst = 0.0f;
    for (size_t row = 0; row < 3; row++) {
        const float x = words[row * 3 + 0];
        const float y = words[row * 3 + 1];
        const float z = words[row * 3 + 2];
        const float length = std::sqrt(x * x + y * y + z * z);
        worst = std::max(worst, std::fabs(length - 1.0f));
    }
    return worst;
}

float TransformShape::deltaOf(const float* before, const float* after) {
    float worst = 0.0f;
    for (size_t word = 0; word < kWords; word++) {
        worst = std::max(worst, std::fabs(after[word] - before[word]));
    }
    return worst;
}

bool TransformShape::moved(const float* before, const float* after) {
    return deltaOf(before, after) > kMotionEpsilon;
}

bool TransformShape::sameShapeAs(const std::vector<uint32_t>& kept, uint32_t candidate) {
    for (const uint32_t other : kept) {
        const uint32_t gap = candidate > other ? candidate - other : other - candidate;
        if (gap < spanBytes()) {
            return true;
        }
    }
    return false;
}

} // namespace wiiuport::title
