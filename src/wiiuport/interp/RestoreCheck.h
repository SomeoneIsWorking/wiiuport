#pragma once

#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/frame/FramePresenter.h"
#include "wiiuport/interp/TickProbe.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace wiiuport::interp {

// Whether the guest's frame comes back exactly after an in-between frame has
// been drawn over it, shown as two images to be compared byte for byte.
//
// One tick is captured twice: the guest's frame as it stood before anything
// of the runtime's was drawn, presented once more to capture it, and then
// either the frame the guest's own swap shows after the restore -- which must
// be identical -- or the in-between frame, which on a moving scene must not
// be. The second is the control: a comparison that finds the in-between
// frame identical to the guest's is not looking at what it thinks it is.
//
// The extra present shows the guest's frame one vblank early in the tick it
// runs in, which is why it runs only when asked.
class RestoreCheck final : public TickProbe {
  public:
    enum class Against : uint8_t {
        Restored,
        InBetween
    };
    // Slot 0 holds the guest's frame as drawn and slot 1 what it is compared
    // against, as in a null diff.
    static constexpr size_t kGuestSlot = 0;
    static constexpr size_t kCheckSlot = 1;

    RestoreCheck(frame::FramePresenter& presenter, frame::FrameCapture& capture);

    // Safe from any thread; runs at the next interpolated tick. False when a
    // check is already waiting to run.
    bool arm(Against against);

    // Every check armed ends here or in `refused`: a capture that could not be
    // armed leaves a slot holding some other frame.
    uint64_t completed() const {
        return m_completed.load();
    }

    uint64_t refused() const {
        return m_refused.load();
    }

    void beforeInBetween(uint64_t tick) override;
    void beforeInBetweenPresent() override;
    void beforeGuestFrameCopied() override;
    void afterTick() override;

  private:
    enum class State : uint8_t {
        Idle,
        ArmedRestored,
        ArmedInBetween
    };
    // Captures the next copy to the scan buffer into `slot`. The capture is
    // taken at that copy, so it has to be armed before the copy it means.
    void capture(size_t slot);

    frame::FramePresenter& m_presenter;
    frame::FrameCapture& m_capture;
    std::atomic<State> m_requested{State::Idle};
    // Owned by the thread that draws, for the tick the check runs in.
    State m_running{State::Idle};
    bool m_failed{false};
    bool m_checkCaptured{false};
    std::atomic<uint64_t> m_completed{0};
    std::atomic<uint64_t> m_refused{0};
};

} // namespace wiiuport::interp
