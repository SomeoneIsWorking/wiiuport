#pragma once

#include "wiiuport/frame/RecordingObserver.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace wiiuport::frame {

// How evenly frames are handed to the display: the time between every two
// presents the renderer issues, the title's and the runtime's alike, taken as
// it returns from each. This is the frame time tools such as MangoHud report.
//
// The counters of ticks interpolated cannot show it: every tick can carry an
// in-between frame and still hand both of its frames over a millisecond
// apart. It is not scan-out: with spare swapchain images a present returns
// at once and the FIFO queue shows each frame at a later refresh, so uneven
// halves here can still reach the screen evenly while the queue never runs
// dry. The renderer has no view of scan-out times to measure that directly.
//
// The same measure is taken of scan-out where the surface reports it: an
// instance fed onScannedOut, by the presentation engine's times of each frame
// reaching the screen, which is what a player sees. One instance measures one
// of the two.
//
// Fed on the rendering thread, read and restarted from the control channel.
class PresentPacing final : public DisplayedListener, public ScanOutListener {
  public:
    using Clock = std::chrono::steady_clock;
    using Now = Clock::time_point (*)();

    // Intervals kept for percentiles since the last restart; later ones are
    // counted and not kept. Four and a half minutes at 60 Hz.
    static constexpr size_t kMaxIntervals = 1u << 14;

    explicit PresentPacing(Now now);

    void onDisplayed(bool fromRuntime) override;
    // Two frames' times are an interval only on the same clock.
    void onScannedOut(const LatteFrameHooks::ShownFrame& shown) override;

    // Forgets every interval, so a measurement covers only what follows.
    void restart();

    struct Summary {
        uint64_t guestFrames{0};
        uint64_t runtimeFrames{0};
        // Intervals measured, and of those the ones kept for percentiles.
        uint64_t intervals{0};
        uint64_t intervalsKept{0};
        std::chrono::microseconds p50{0};
        std::chrono::microseconds p95{0};
        std::chrono::microseconds p99{0};
        std::chrono::microseconds longest{0};
        // Medians of the two halves of an interpolated tick: from the title's
        // frame to the runtime's, and from the runtime's to the title's. Even
        // pacing puts both at half a tick.
        std::chrono::microseconds guestToRuntimeMedian{0};
        std::chrono::microseconds runtimeToGuestMedian{0};
        // For scan-out, how far along its way the time was taken; none
        // before the first frame is reported.
        std::optional<LatteFrameHooks::ShownStage> stage;
    };

    Summary summary() const;

  private:
    struct Interval {
        std::chrono::microseconds length;
        bool fromRuntime;
        bool toRuntime;
    };

    // `at` since any epoch of `clock`'s, one clock per source.
    void record(bool fromRuntime, std::chrono::nanoseconds at, uint64_t clock);

    Now m_now;
    mutable std::mutex m_mutex;
    bool m_seenOne{false};
    std::chrono::nanoseconds m_last{0};
    uint64_t m_lastClock{0};
    std::optional<LatteFrameHooks::ShownStage> m_stage;
    bool m_lastFromRuntime{false};
    uint64_t m_guestFrames{0};
    uint64_t m_runtimeFrames{0};
    uint64_t m_intervals{0};
    std::vector<Interval> m_kept;
};

} // namespace wiiuport::frame
