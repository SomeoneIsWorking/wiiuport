#pragma once

#include "wiiuport/frame/FramePresenter.h"
#include "wiiuport/frame/FrameReplayer.h"
#include "wiiuport/frame/GuestStateGuard.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/frame/ReplayScheduler.h"
#include "wiiuport/interp/CutDetector.h"
#include "wiiuport/interp/ObjectBlend.h"
#include "wiiuport/interp/TickProbe.h"
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
// frame before, and presented; then put back and copied to the scan buffer,
// so the guest's own swap that follows presents the frame the guest drew.
//
// Spacing the two presents is the display's job, not this class's: with FIFO
// presentation each lands on its own vblank, so a 30 Hz title shows sixty
// evenly spaced frames. Sleeping this thread to space them instead holds back
// the title's next frame behind the sleep -- measured: 30.10 Hz without
// interpolation fell to 20 Hz with a half-tick wait here.
//
// Putting the guest's frame back is not a luxury: the in-between replay drew
// over every render target the frame uses. Leaving it would show the
// in-between frame twice and hand the title's next frame render targets its
// own frame did not leave behind. It is put back by copying what the replay
// overwrote, which costs transfers; drawing the frame a second time cost a
// third of the tick. When the copies cannot undo everything the replay did,
// the frame is drawn again after all, and that is counted.
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
                           frame::FramePresenter& presenter, frame::GuestStateGuard& guard,
                           const frame::ReplayScheduler& scheduler, TickProbe& probe, Now now);

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

    // How each in-between frame was taken back out of guest-visible state.
    // By copy is the intended way; by replay means the copies could not
    // undo something the in-between replay did, which `notCopied` says.
    uint64_t restoresByCopy() const {
        return m_restoresByCopy;
    }

    uint64_t restoresByReplay() const {
        return m_restoresByReplay;
    }

    // Texture subresources copied back, over every restore.
    uint64_t subresourcesRestored() const {
        return m_subresourcesRestored;
    }

    // Copies aside allocated rather than reused: steady when the title writes
    // the same targets every frame.
    uint64_t shadowsCreated() const {
        return m_shadowsCreated;
    }

    // Summed over the restores that fell back to a replay.
    struct NotCopied {
        uint64_t subresources{0};
        uint64_t texturesCreated{0};
        uint64_t streamoutWrites{0};
    };

    const NotCopied& notCopied() const {
        return m_notCopied;
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
    // Puts the frame back as the guest drew it and copies it to the scan
    // buffer.
    void restoreGuestFrame(const frame::FrameRecording& recording);

    const ViewTracker& m_tracker;
    TransformSubstitution& m_substitution;
    ObjectBlend& m_objects;
    frame::FrameReplayer& m_replayer;
    frame::FramePresenter& m_presenter;
    frame::GuestStateGuard& m_guard;
    const frame::ReplayScheduler& m_scheduler;
    TickProbe& m_probe;
    Now m_now;
    CutDetector m_cuts;
    std::atomic<bool> m_enabled{false};
    uint64_t m_ticks{0};
    uint64_t m_framesInterpolated{0};
    std::array<uint64_t, kSkipCount> m_skipped{};
    uint64_t m_restoresRefused{0};
    uint64_t m_restoresByCopy{0};
    uint64_t m_restoresByReplay{0};
    uint64_t m_subresourcesRestored{0};
    uint64_t m_shadowsCreated{0};
    NotCopied m_notCopied;
    std::array<std::chrono::nanoseconds, kPhaseCount> m_timeIn{};
};

} // namespace wiiuport::interp
