#pragma once

#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/frame/FramePresenter.h"
#include "wiiuport/frame/FrameReplayer.h"
#include "wiiuport/frame/RecordingObserver.h"

#include <cstddef>
#include <cstdint>

namespace wiiuport::frame {

// Runs the replayer at the one safe moment: after a frame has been published,
// and presents what it drew.
//
// The present is not optional: replayed draws land in the colour buffer the
// guest is about to overdraw, so a replay nobody presents is never seen. That
// is what the first null diff measured -- a replay arm 0.05 percentage points
// from a no-replay control -- and it is why the two belong to one owner.
//
// It exists so the recorder stays a recorder and the replayer stays free of
// any opinion about when it runs. Everything it holds is a reference to an
// owner that outlives it.
class ReplayScheduler final : public FrameShownListener {
  public:
    // Which slot each half of a null diff lands in. Fixed here because this
    // is the only code that knows which present is which.
    static constexpr size_t kTitleSlot = 0;
    static constexpr size_t kReplaySlot = 1;

    ReplayScheduler(FrameReplayer& replayer, FramePresenter& presenter, FrameCapture& capture);

    // Capture one frame twice: as the title presented it, and as a replay of
    // that same frame redrew it. Both halves are armed here because only the
    // frame boundary makes them the same frame -- the title's present happens
    // immediately before the frame end, and the replay's immediately after.
    // Comparing captures taken seconds apart measures the scene moving, which
    // is what the first attempt at this measured.
    //
    // Runs over two frame boundaries, both on the thread that owns them.
    // An earlier version armed the title's capture from the HTTP thread,
    // where it could be consumed by any swap: the control came out at 7,604
    // differing bytes on one run and zero on the next, which is a race and
    // not a measurement. Arming it at a frame end instead fixes which frame
    // it belongs to.
    //
    // `redraw` false runs the same sequence without replaying anything, so
    // the runtime re-presents the colour buffer the title just presented.
    // That control must come out byte-identical; if it does not, the
    // difference is in the capture or present path and no number this
    // produces about a replay can be believed. The first run of it mattered:
    // a redrawing null diff reported exactly the 7,604 differing bytes an
    // unrelated earlier measurement had, which is not how a real difference
    // behaves.
    //
    // False when a replay or another null diff is already armed, rather than
    // quietly taking over one somebody else asked for.
    bool armNullDiff(bool redraw);

    bool nullDiffPending() const {
        return m_phase != Phase::Idle;
    }

    uint64_t nullDiffsCompleted() const {
        return m_nullDiffsCompleted;
    }

    void onFrameShown(const FrameRecording& recording) override;

  private:
    FrameReplayer& m_replayer;
    FramePresenter& m_presenter;
    // Requested: arm the title's capture at the next frame end, so it is
    // consumed by the swap that ends the frame after. Capturing: that swap
    // has happened, so this frame end replays into the same colour buffer
    // and presents it.
    enum class Phase : uint8_t {
        Idle,
        Requested,
        Capturing
    };

    FrameCapture& m_capture;
    Phase m_phase{Phase::Idle};
    bool m_redrawPending{true};
    uint64_t m_nullDiffsCompleted{0};
};

} // namespace wiiuport::frame
