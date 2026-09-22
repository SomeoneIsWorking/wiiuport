#pragma once

#include "wiiuport/frame/RecordingObserver.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
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
// Fed on the rendering thread, read and restarted from the control channel.
class PresentPacing final : public DisplayedListener {
  public:
    using Clock = std::chrono::steady_clock;
    using Now = Clock::time_point (*)();

    // Intervals kept for percentiles since the last restart; later ones are
    // counted and not kept. Four and a half minutes at 60 Hz.
    static constexpr size_t kMaxIntervals = 1u << 14;

    explicit PresentPacing(Now now);

    void onDisplayed(bool fromRuntime) override;

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
    };

    Summary summary() const;

  private:
    struct Interval {
        std::chrono::microseconds length;
        bool fromRuntime;
        bool toRuntime;
    };

    Now m_now;
    mutable std::mutex m_mutex;
    bool m_seenOne{false};
    Clock::time_point m_last;
    bool m_lastFromRuntime{false};
    uint64_t m_guestFrames{0};
    uint64_t m_runtimeFrames{0};
    uint64_t m_intervals{0};
    std::vector<Interval> m_kept;
};

} // namespace wiiuport::frame
