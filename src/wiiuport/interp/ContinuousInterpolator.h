#pragma once

#include "wiiuport/frame/FramePresenter.h"
#include "wiiuport/frame/FrameReplayer.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/frame/ReplayScheduler.h"
#include "wiiuport/interp/CutDetector.h"
#include "wiiuport/interp/ObjectBlend.h"
#include "wiiuport/interp/TransformSubstitution.h"
#include "wiiuport/interp/ViewTracker.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace wiiuport::interp {

// Shows a frame of the runtime's own between every two the title draws.
//
// Runs at the moment the guest's frame is finished and not yet shown. The
// frame it just drew is replayed with the view blended half way back to the
// frame before, and presented; then replayed as drawn and copied back to the
// scan buffer, so the guest's own swap that follows presents the frame the
// guest drew.
//
// Spacing the two presents is the display's job, not this class's: with FIFO
// presentation each lands on its own vblank, so a 30 Hz title shows sixty
// evenly spaced frames. Sleeping this thread to space them instead holds back
// the title's next frame behind the sleep -- measured: 30.10 Hz without
// interpolation fell to 20 Hz with a half-tick wait here.
//
// The replay that puts the guest's frame back is not a luxury: the in-between
// replay drew over every render target the frame uses. Leaving it would show
// the in-between frame twice and hand the title's next frame render targets
// its own frame did not leave behind.
//
// Every tick is counted, and every tick without an in-between frame is
// counted by why. A run where this never fired has to be distinguishable from
// one where it did, from the numbers alone.
class ContinuousInterpolator final : public frame::FrameEndListener {
  public:
    using Clock = std::chrono::steady_clock;
    // Injected so a test can time the phases without a real clock.
    using Now = Clock::time_point (*)();

    // Where between the two ticks the in-between frame sits. Half, because
    // it is shown one vblank -- half a 30 Hz tick -- before the frame it
    // leads into.
    static constexpr float kBlendPoint = 0.5f;

    enum class Skip : uint32_t {
        Disabled,
        // A one-shot replay or null diff owns the next frame boundary.
        OneShotInFlight,
        IncompleteRecording,
        // The title has not been seen presenting to the TV yet.
        NoPresentSeen,
        NoView,
        CameraCut,
        ReplayDrewNothing,
        PresentRefused,
        Count
    };
    static constexpr size_t kSkipCount = static_cast<size_t>(Skip::Count);
    static std::string_view skipName(Skip skip);

    ContinuousInterpolator(const ViewTracker& tracker, TransformSubstitution& substitution,
                           ObjectBlend& objects, frame::FrameReplayer& replayer,
                           frame::FramePresenter& presenter,
                           const frame::ReplayScheduler& scheduler, Now now);

    // Last among the frame-recorded listeners: the view tracker and the object
    // blend have to have taken this frame in first.
    void onFrameRecorded(const frame::FrameRecording& recording) override;

    // Safe from any thread; takes effect at the next frame.
    void setEnabled(bool enabled) {
        m_enabled.store(enabled);
        m_objects.setPlanning(enabled);
    }

    bool enabled() const {
        return m_enabled.load();
    }

    uint64_t ticks() const {
        return m_ticks;
    }

    uint64_t framesInterpolated() const {
        return m_framesInterpolated;
    }

    uint64_t skipped(Skip skip) const {
        return m_skipped[static_cast<size_t>(skip)];
    }

    // The guest's frame could not be copied back after an in-between frame,
    // so the next present shows the in-between frame a second time.
    uint64_t restoresRefused() const {
        return m_restoresRefused;
    }

    // Where an interpolated tick's time goes, summed over every interpolated
    // tick. The title's own frame waits on all of it, so a tick rate that
    // collapses is attributed to a phase here rather than guessed at.
    enum class Phase : uint32_t {
        BlendedReplay,
        Present,
        Restore,
        Count
    };
    static constexpr size_t kPhaseCount = static_cast<size_t>(Phase::Count);
    static std::string_view phaseName(Phase phase);

    std::chrono::nanoseconds timeIn(Phase phase) const {
        return m_timeIn[static_cast<size_t>(phase)];
    }

    const CutDetector& cuts() const {
        return m_cuts;
    }

    const ObjectBlend& objects() const {
        return m_objects;
    }

  private:
    void skip(Skip reason);
    // Adds the time since `since` to `phase`, and returns now.
    Clock::time_point charge(Phase phase, Clock::time_point since);
    // Replays the frame as the guest drew it and copies it to the scan buffer.
    void restoreGuestFrame(const frame::FrameRecording& recording);

    const ViewTracker& m_tracker;
    TransformSubstitution& m_substitution;
    ObjectBlend& m_objects;
    frame::FrameReplayer& m_replayer;
    frame::FramePresenter& m_presenter;
    const frame::ReplayScheduler& m_scheduler;
    Now m_now;
    CutDetector m_cuts;
    std::atomic<bool> m_enabled{false};
    uint64_t m_ticks{0};
    uint64_t m_framesInterpolated{0};
    std::array<uint64_t, kSkipCount> m_skipped{};
    uint64_t m_restoresRefused{0};
    std::array<std::chrono::nanoseconds, kPhaseCount> m_timeIn{};
};

} // namespace wiiuport::interp
