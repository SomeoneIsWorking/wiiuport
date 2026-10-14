#pragma once

#include "wiiuport/title/DrawInterpolation.h"

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's grappling rope (Himo2) in the in-between draw phase.
//
// daHimo2_c's execute (0x0216e194) places the hand, the hook and each rope segment; its draw
// (0x0216ca5c) builds the line from them and eases the rope's sway toward the hook's movement since
// its last draw. For the gated draw phase the hand, hook and segments are at the midpoint of the
// last two ticks', and the sway is put back after it so it eases once a tick, not once a draw
// phase.
class RopeInterpolation final : public DrawInterpolation::DrawPhaseListener {
  public:
    // base_process_class's process name, s16 in the high half.
    static constexpr uint32_t kProcessName = 0x8;
    static constexpr uint32_t kHimo2 = 446;
    // The rope's end, then the index of its first live segment.
    static constexpr uint32_t kRopeEnd = 0x3d0;
    static constexpr uint32_t kFirstSegment = 0x3e8;
    // Hand, then hook.
    static constexpr uint32_t kHandHook = 0x408;
    static constexpr uint32_t kHandHookWords = 6;
    static constexpr uint32_t kSegments = 0x450;
    static constexpr uint32_t kSegmentStride = 0x24;
    static constexpr uint32_t kSegmentPosition = 0x10;
    static constexpr uint32_t kSegmentCount = 0x62;
    static constexpr uint32_t kSegmentWords = kSegmentCount * kSegmentStride / 4;
    // The draw's own state: settle per sway segment, the hook where it last drew, the sway angles.
    static constexpr uint32_t kSway = 0x21d8;
    static constexpr uint32_t kSwayWords = 10;
    // The hook's eased tilt.
    static constexpr uint32_t kTilt = 0x2388;

    struct Seams {
        DrawInterpolation::ReadWords readWords;
        DrawInterpolation::WriteWords writeWords;
    };

    explicit RopeInterpolation(Seams seams);

    void onDrawPhaseBegin(uint64_t tick) override;
    void onDrawPhaseEnd() override;
    void onActorDraw(uint32_t actor) override;

    std::string json() const;

  private:
    // The rope as a tick left it.
    struct Seen {
        uint64_t tick = 0;
        uint32_t firstSegment = 0;
        std::array<uint32_t, 3> end{};
        std::array<uint32_t, kHandHookWords> handHook{};
        std::vector<uint32_t> segments;
    };

    struct Restore {
        uint32_t address = 0;
        std::vector<uint32_t> words;
    };

    // The words the draw phase sees, and the tick's own to put back after it.
    struct Change {
        std::vector<uint32_t> blended;
        std::vector<uint32_t> original;
    };

    bool read(uint32_t actor, Seen& seen);
    bool blend(uint32_t actor, const Seen& previous, const Seen& current);
    void hold(uint32_t address, uint32_t count);
    void write(uint32_t address, Change change);

    DrawInterpolation::ReadWords m_readWords;
    DrawInterpolation::WriteWords m_writeWords;

    mutable std::mutex m_mutex;
    uint64_t m_tick = 0;
    std::map<uint32_t, Seen> m_seen;
    std::vector<Restore> m_restores;

    uint64_t m_draws = 0;
    uint64_t m_blends = 0;
    uint64_t m_firstSeen = 0;
    uint64_t m_unblendable = 0;
    uint64_t m_unreadable = 0;
    uint64_t m_writeFailures = 0;
    uint64_t m_restored = 0;
};

} // namespace wiiuport::title
