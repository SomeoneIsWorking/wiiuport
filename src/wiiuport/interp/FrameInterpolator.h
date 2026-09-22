#pragma once

#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/frame/ReplayScheduler.h"
#include "wiiuport/interp/TransformSearch.h"
#include "wiiuport/interp/TransformSubstitution.h"

#include <cstdint>
#include <string>

namespace wiiuport::interp {

// Produces one extra frame between two the title drew.
//
// It is the only place that knows an interpolated frame is three things at
// once: the view the search found, a blend of that view's last two values,
// and a replay of the last frame's geometry drawn with it. Keeping that
// sequence here is what lets the search stay a pure analysis, the
// substitution stay a writer of twelve floats, and the scheduler stay
// ignorant of what a replay is for.
//
// It arms; it never presents. The frame boundary owns that, and it is also
// where the blend is taken back down: a substitution left armed would write
// the camera of a frame that is over into whatever replay ran next.
class FrameInterpolator final : public frame::FrameEndListener {
  public:
    FrameInterpolator(const TransformSearch& search, TransformSubstitution& substitution,
                      frame::ReplayScheduler& scheduler);

    // Arms one interpolated frame at `t` between the two most recent views.
    // False when there is nothing to blend, when `t` is outside the two
    // frames, or when a replay is already in flight; `lastRefusal` then says
    // which, because all three otherwise look like a frame that changed
    // nothing.
    bool armOnce(float t);

    // Empty when the last arming succeeded.
    const std::string& lastRefusal() const {
        return m_lastRefusal;
    }

    uint64_t framesArmed() const {
        return m_framesArmed;
    }

    uint64_t framesRefused() const {
        return m_framesRefused;
    }

    // Disarms the blend once the frame it was armed for has been replayed.
    // Reading the scheduler rather than counting frames is what keeps this
    // right when a frame end arrives that the scheduler did nothing with.
    void onFrameRecorded(const frame::FrameRecording& recording) override;

    const TransformSubstitution& substitution() const {
        return m_substitution;
    }

  private:
    bool refuse(std::string reason);

    const TransformSearch& m_search;
    TransformSubstitution& m_substitution;
    frame::ReplayScheduler& m_scheduler;
    std::string m_lastRefusal;
    uint64_t m_framesArmed{0};
    uint64_t m_framesRefused{0};
};

} // namespace wiiuport::interp
