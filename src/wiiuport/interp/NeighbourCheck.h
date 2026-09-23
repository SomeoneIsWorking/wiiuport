#pragma once

#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/interp/TickProbe.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace wiiuport::interp {

// The in-between frame and the title's frames either side of it, shown as
// three images, so whether it lies between them is judged from what was
// drawn rather than from the values handed to the draws.
//
// The title's frame of one interpolated tick is captured at its copy to the
// scan buffer; the next tick's in-between frame at its present, and its
// title's frame at its copy. The two ticks must be consecutive title frames:
// the in-between frame blends its tick's frame with the one before, so a
// tick not interpolated between them would make the first capture another
// frame's. When that happens the check starts over from the later tick.
//
// It presents nothing extra: every image is one the display was shown.
class NeighbourCheck final : public TickProbe {
  public:
    // Past the restore check's two slots, so the checks may run together.
    static constexpr size_t kBeforeSlot = 2;
    static constexpr size_t kInBetweenSlot = 3;
    static constexpr size_t kAfterSlot = 4;
    static_assert(kAfterSlot < frame::FrameCapture::kSlotCount);

    explicit NeighbourCheck(frame::FrameCapture& capture);

    // Safe from any thread; runs over the next two consecutive interpolated
    // ticks. False when a check is already waiting to run.
    bool arm();

    // Every check armed ends here or in `refused`: a capture that could not
    // be armed leaves a slot holding some other frame.
    uint64_t completed() const {
        return m_completed.load();
    }

    uint64_t refused() const {
        return m_refused.load();
    }

    // Checks that started over because the tick after their first was not
    // interpolated.
    uint64_t restarted() const {
        return m_restarted.load();
    }

    void beforeInBetween(uint64_t tick) override;
    void beforeInBetweenPresent() override;
    void beforeGuestFrameCopied() override;
    void afterTick() override;

  private:
    enum class Stage : uint8_t {
        Idle,
        // The title's frame of this tick is the one before.
        Before,
        // This tick's in-between frame and its title's frame follow it.
        Following
    };

    void capture(size_t slot);
    void finish(bool completed);

    frame::FrameCapture& m_capture;
    std::atomic<bool> m_requested{false};
    // Owned by the thread that draws.
    Stage m_stage{Stage::Idle};
    Stage m_running{Stage::Idle};
    uint64_t m_beforeTick{0};
    bool m_failed{false};
    size_t m_captured{0};
    std::atomic<uint64_t> m_completed{0};
    std::atomic<uint64_t> m_refused{0};
    std::atomic<uint64_t> m_restarted{0};
};

} // namespace wiiuport::interp
