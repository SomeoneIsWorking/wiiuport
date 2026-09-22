#include "wiiuport/interp/FrameInterpolator.h"

namespace wiiuport::interp {

FrameInterpolator::FrameInterpolator(const TransformSearch& search,
                                     TransformSubstitution& substitution,
                                     frame::ReplayScheduler& scheduler)
    : m_search(search), m_substitution(substitution), m_scheduler(scheduler) {
}

bool FrameInterpolator::armOnce(float t) {
    if (!(t >= 0.0f) || !(t <= 1.0f)) {
        // Written against NaN as well as a number out of range: a NaN blend
        // writes a transform that draws nothing, which reads as a frame the
        // runtime failed to replay.
        return refuse("blend point outside the two frames");
    }
    std::vector<ViewSlot> slots = m_search.viewSlots();
    if (slots.empty()) {
        return refuse("no view transform found yet");
    }
    // The scheduler is asked first: it is the one that can refuse, and
    // arming the blend before it would leave a frame somebody else asked for
    // holding this one's camera.
    if (!m_scheduler.armNullDiff(true)) {
        return refuse("a replay is already in flight");
    }
    if (!m_substitution.armOnce(slots, t)) {
        // Unreachable while the slot list is non-empty, and reported rather
        // than ignored: the replay now in flight would redraw the title's own
        // frame and look like interpolation that changed nothing.
        return refuse("the substitution refused the slots");
    }
    m_lastRefusal.clear();
    ++m_framesArmed;
    return true;
}

void FrameInterpolator::onFrameRecorded(const frame::FrameRecording&) {
    if (m_substitution.isArmed() && !m_scheduler.nullDiffPending()) {
        m_substitution.disarm();
    }
}

bool FrameInterpolator::refuse(std::string reason) {
    m_lastRefusal = std::move(reason);
    ++m_framesRefused;
    return false;
}

} // namespace wiiuport::interp
