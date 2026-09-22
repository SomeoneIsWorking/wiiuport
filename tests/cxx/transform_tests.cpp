#include "check.h"
#include "suites.h"
#include "wiiuport/interp/Transform3x4.h"

#include <array>
#include <cmath>

using wiiuport::interp::Transform3x4;

namespace {

std::array<float, 12> aroundZ(float radians, float tx, float ty, float tz) {
    float c = std::cos(radians);
    float s = std::sin(radians);
    return {c, -s, 0.0f, tx, s, c, 0.0f, ty, 0.0f, 0.0f, 1.0f, tz};
}

void endpointsAreExact() {
    auto a = Transform3x4::fromRowMajor(aroundZ(0.2f, 1.0f, 2.0f, 3.0f).data());
    auto b = Transform3x4::fromRowMajor(aroundZ(0.9f, 7.0f, 8.0f, 9.0f).data());
    // A blend that silently did nothing would still pass a midpoint check that
    // only asked for something "between", so both ends are pinned exactly.
    check::isTrue(Transform3x4::blend(a, b, 0.0f) == a, "t=0 returns the first input unchanged");
    check::isTrue(Transform3x4::blend(a, b, 1.0f) == b, "t=1 returns the second input unchanged");
}

void translationIsLinear() {
    auto a = Transform3x4::fromRowMajor(aroundZ(0.0f, 0.0f, 0.0f, 0.0f).data());
    auto b = Transform3x4::fromRowMajor(aroundZ(0.0f, 10.0f, -20.0f, 40.0f).data());
    auto mid = Transform3x4::blend(a, b, 0.5f);
    check::near(mid.translation().x, 5.0f, 1e-5f, "midpoint x");
    check::near(mid.translation().y, -10.0f, 1e-5f, "midpoint y");
    check::near(mid.translation().z, 20.0f, 1e-5f, "midpoint z");
    auto quarter = Transform3x4::blend(a, b, 0.25f);
    check::near(quarter.translation().x, 2.5f, 1e-5f, "quarter x");
}

void blendedRotationStaysARotation() {
    // The reason this is not an elementwise lerp. Averaging the two matrices
    // gives rows shorter than one, which would shrink the world a little on
    // every in-between frame.
    auto a = Transform3x4::fromRowMajor(aroundZ(0.0f, 0.0f, 0.0f, 0.0f).data());
    auto b = Transform3x4::fromRowMajor(aroundZ(1.5f, 0.0f, 0.0f, 0.0f).data());
    auto mid = Transform3x4::blend(a, b, 0.5f);
    check::near(mid.rotationError(), 0.0f, 1e-5f, "blended rotation is still orthonormal");

    std::array<float, 12> averaged{};
    for (int i = 0; i < 12; i++) {
        averaged[i] = (a.values()[i] + b.values()[i]) * 0.5f;
    }
    float naiveError = Transform3x4::fromRowMajor(averaged.data()).rotationError();
    check::isTrue(naiveError > 0.1f,
                  "an elementwise average of the same pair is measurably not a rotation");
}

void halfwayIsTheHalfAngle() {
    auto a = Transform3x4::fromRowMajor(aroundZ(0.0f, 0.0f, 0.0f, 0.0f).data());
    auto b = Transform3x4::fromRowMajor(aroundZ(1.2f, 0.0f, 0.0f, 0.0f).data());
    auto mid = Transform3x4::blend(a, b, 0.5f);
    auto expected = Transform3x4::fromRowMajor(aroundZ(0.6f, 0.0f, 0.0f, 0.0f).data());
    for (int i = 0; i < 12; i++) {
        check::near(mid.values()[i], expected.values()[i], 1e-5f,
                    "halfway equals the half-angle rotation");
    }
}

void theLongWayRoundIsNotTaken() {
    // Two rotations more than pi apart, so the short arc is the one through
    // +/-pi rather than the one through identity. Below pi the direct path is
    // already the short one and this would prove nothing -- the first version
    // of this test used 3.0 radians and failed for exactly that reason.
    auto a = Transform3x4::fromRowMajor(aroundZ(-2.0f, 0, 0, 0).data());
    auto b = Transform3x4::fromRowMajor(aroundZ(2.0f, 0, 0, 0).data());
    auto mid = Transform3x4::blend(a, b, 0.5f);
    // Halfway along the short arc is a half turn, cos(pi) = -1. The long way
    // would land on identity at +1, so the two answers are as far apart as
    // this test can put them.
    check::near(mid.values()[0], -1.0f, 1e-4f,
                "halfway is a half turn, so the short arc was taken");
    check::near(mid.rotationError(), 0.0f, 1e-5f, "and the result is still a rotation");
}

void outOfRangeIsClamped() {
    auto a = Transform3x4::fromRowMajor(aroundZ(0.0f, 0.0f, 0.0f, 0.0f).data());
    auto b = Transform3x4::fromRowMajor(aroundZ(0.5f, 4.0f, 0.0f, 0.0f).data());
    check::isTrue(Transform3x4::blend(a, b, -1.0f) == a, "t below zero clamps to the first input");
    check::isTrue(Transform3x4::blend(a, b, 2.0f) == b, "t above one clamps to the second input");
}

void rotationErrorSeesAScale() {
    auto scaled = aroundZ(0.3f, 0.0f, 0.0f, 0.0f);
    for (float& value : scaled) {
        value *= 2.0f;
    }
    check::isTrue(Transform3x4::fromRowMajor(scaled.data()).rotationError() > 0.9f,
                  "a doubled matrix is reported as not a rotation");
    check::near(Transform3x4::fromRowMajor(aroundZ(0.3f, 5.0f, 6.0f, 7.0f).data()).rotationError(),
                0.0f, 1e-6f, "translation does not affect the rotation error");
}

void aViewThatHeldStillBlendsToItselfExactly() {
    // A camera that did not move must give an in-between frame byte-identical
    // to the title's, which rounding through two rigid inverses and a slerp
    // does not: every held view is checked bit for bit.
    for (int step = 0; step < 64; ++step) {
        auto angle = static_cast<float>(step) * 0.1f;
        auto view = Transform3x4::fromRowMajor(
            aroundZ(angle, 300000.0f + angle, -1234.5f * angle, 17.0f).data());
        check::isTrue(Transform3x4::blendView(view, view, 0.5f) == view,
                      "a held view blends to itself, bit for bit");
        check::isTrue(Transform3x4::blend(view, view, 0.5f) == view,
                      "and so does a held transform");
    }
}

} // namespace

namespace wiiuport::tests {

void runTransformTests() {
    endpointsAreExact();
    translationIsLinear();
    blendedRotationStaysARotation();
    halfwayIsTheHalfAngle();
    theLongWayRoundIsNotTaken();
    outOfRangeIsClamped();
    rotationErrorSeesAScale();
    aViewThatHeldStillBlendsToItselfExactly();
}

} // namespace wiiuport::tests
