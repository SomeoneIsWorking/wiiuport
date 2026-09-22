#include "wiiuport/interp/CutDetector.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wiiuport::interp {

float CutDetector::medianStep() const {
    std::array<float, kWindow> sorted = m_steps;
    auto middle = sorted.begin() + static_cast<std::ptrdiff_t>(m_stepCount / 2);
    std::nth_element(sorted.begin(), middle,
                     sorted.begin() + static_cast<std::ptrdiff_t>(m_stepCount));
    return *middle;
}

bool CutDetector::isCut(const Transform3x4& before, const Transform3x4& after) {
    if (Transform3x4::rotationAngleBetween(before, after) > kMaxTurnRadians) {
        ++m_cutsByTurn;
        return true;
    }
    Vec3 from = before.rigidInverse().translation();
    Vec3 to = after.rigidInverse().translation();
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float dz = to.z - from.z;
    float step = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
    float reach = std::max({std::fabs(from.x), std::fabs(from.y), std::fabs(from.z),
                            std::fabs(to.x), std::fabs(to.y), std::fabs(to.z)});
    // A camera holding still is not evidence about how far it moves, and a
    // median of rounding would call the first real move a cut.
    if (step <= kNoiseUlps * std::numeric_limits<float>::epsilon() * reach) {
        return false;
    }
    bool cut = m_stepCount >= kMinimumMoves && step > kStepFactor * medianStep();
    remember(step);
    if (cut) {
        ++m_cutsByStep;
    }
    return cut;
}

void CutDetector::remember(float step) {
    m_steps[m_next] = step;
    m_next = (m_next + 1) % kWindow;
    m_stepCount = std::min(m_stepCount + 1, kWindow);
}

} // namespace wiiuport::interp
