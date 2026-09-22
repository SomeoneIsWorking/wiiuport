#pragma once

#include "wiiuport/interp/Transform3x4.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace wiiuport::interp {

// Tells a camera that moved from a camera that cut.
//
// Blending across a cut draws a frame from a viewpoint the title never had --
// half way between two unrelated shots -- which is worse than no in-between
// frame at all. Two measures, both of the camera itself rather than of the
// matrix's raw columns: how far it turned, and how far its position moved
// against how far it has been moving. The position is recovered from the view
// transform, because the translation column alone also swings when the camera
// only turns.
//
// Pure, and blind to what the title is: the thresholds are in the camera's own
// terms, turning or moving far beyond anything it did recently.
class CutDetector {
  public:
    // A quarter turn per tick is 1,350 degrees a second at 30 Hz: no camera a
    // player steers turns that fast, and every cut to a new angle does.
    static constexpr float kMaxTurnRadians = 0.785398163f;
    // Moving this many times further than the recent median is a jump.
    static constexpr float kStepFactor = 8.0f;
    // Below this many recorded moves the median is not an estimate yet, and
    // no move is called a jump by distance.
    static constexpr size_t kMinimumMoves = 4;
    static constexpr size_t kWindow = 16;
    // A camera position recovered from a float view is only known to a few
    // units in its last place, scaled by how far from the origin it stands:
    // Wind Waker's sea puts it 3e5 units out, where one unit in the last place
    // is 0.03. A step inside this many of them is rounding, not motion.
    static constexpr float kNoiseUlps = 16.0f;

    // Whether the move from `before` to `after` is a cut. Every move that is
    // motion is remembered, cuts included: a lone jump barely shifts the
    // median, while a camera that sets off at a new speed moves it within half
    // a window. Remembering only the moves judged smooth would lock the median
    // at the resting speed and call every step of the walk that follows a cut.
    bool isCut(const Transform3x4& before, const Transform3x4& after);

    uint64_t cutsByTurn() const {
        return m_cutsByTurn;
    }

    uint64_t cutsByStep() const {
        return m_cutsByStep;
    }

  private:
    float medianStep() const;
    void remember(float step);

    std::array<float, kWindow> m_steps{};
    size_t m_stepCount{0};
    size_t m_next{0};
    uint64_t m_cutsByTurn{0};
    uint64_t m_cutsByStep{0};
};

} // namespace wiiuport::interp
