#pragma once

#include <cstddef>
#include <cstdint>

namespace wiiuport::title {

// What a run of twelve floats in guest memory is, and what it is not.
//
// **One owner, because two locators ask the same question and got the same answer twice.**
// `NodePoseLocator` reads the node's own memory at the node's draw; `ObjectPoseLocator` reads
// the uniform buffer the title assembled for a named node's draw. Each had its own `isPose`
// and its own copy of the same two tolerances, which is a rule implemented twice and free to
// drift -- and it did drift in the only way that matters: a rule that accepts only a *rigid*
// transform reports "nothing found" identically for a field that is absent and for a field
// that is present and carries scale, and those two point at different places to look next.
//
// So the classification is here, once, in two classes:
//
//   `isRigid` -- three unit rows, pairwise perpendicular. The strict reading, and the one a
//     "rigid 3x4" asks for.
//   `isAffine` -- a non-singular 3x3, whatever its row lengths. The loose reading, and the one
//     that can tell present-and-scaled from absent.
//
// And two measurements that keep a shape from being called a pose: how far it is from unit
// length (`scaleOf`), and whether it changed (`moved`). `moved` is a *magnitude* test and not a
// bitwise one, because the first run of this reported `moved 18` beside `biggest delta
// 0.000000` -- values differing in the last mantissa bit -- and named three static matrices
// that way. See `wiiuport/docs/frame-interpolation.md`.
//
// Every tolerance is a named constant with a stated reason, because these are the numbers a
// reader would want to argue with, and a bar nobody can see the value of is a threshold
// rather than a test.
struct TransformShape {
    // How far a row's length may be from 1 and how far two rows may be from perpendicular
    // before a 3x4 stops being a rigid transform. Loose on purpose: these are the width of a
    // normal-mapping convention and of a matrix built by a hundred multiplies, not a
    // precision claim.
    static constexpr float kUnitTolerance = 0.01f;
    static constexpr float kPerpendicularTolerance = 0.01f;

    // How much a pose has to change between two draws of the same object, which are a frame
    // apart, to count as having moved. A change smaller than this is a rounding difference,
    // and a bar that accepts rounding differences finds a basis matrix in every object.
    static constexpr float kMotionEpsilon = 1e-3f;

    // How non-singular a 3x3 has to be to count as a transform at all. Without a floor a
    // plane of near-zero numbers has a determinant near zero, reads as a matrix, and the loose
    // bar finds one at every offset -- a bar that cannot fail.
    static constexpr double kDeterminantFloor = 1e-6;

    // How many floats make a 3x4. Three rows of three, then the translation.
    static constexpr size_t kWords = 12;

    // Three unit rows, pairwise perpendicular.
    static bool isRigid(const float* words);

    // A non-singular 3x3, whatever its row lengths. The loose class.
    static bool isAffine(const float* words);

    // How far the row lengths are from 1, so a scaled transform is visible as a number rather
    // than being silently excluded by the strict class. Zero is rigid.
    static float scaleOf(const float* words);

    // The largest absolute difference between two readings of the same `kWords` floats.
    static float deltaOf(const float* before, const float* after);

    // Whether that difference is movement rather than rounding.
    static bool moved(const float* before, const float* after);
};

} // namespace wiiuport::title
