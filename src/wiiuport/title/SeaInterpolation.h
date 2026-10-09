#pragma once

#include "wiiuport/title/DrawInterpolation.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's sea surface in the in-between draw phase.
//
// daSea_packet_c::execute fills a 65 x 65 height grid about the player each tick; HD's daSea_Draw
// (0x0246caec) builds the vertices from it in the draw phase and steps a texture scroll counter.
// For the gated draw phase the grid and its corner are at the midpoint of the last two ticks, and
// the counter is put back after it so it steps once a tick, not once a draw phase.
class SeaInterpolation final : public DrawInterpolation::DrawPhaseListener {
  public:
    // Holds the sea packet's address (l_cloth).
    static constexpr uint32_t kSeaPacket = 0x1046d8b0;
    // mInitFlag, then mCullStopFlag: bytes 0 and 1 of this word.
    static constexpr uint32_t kInitFlag = 0x220;
    // mDrawMinX, mDrawMinZ.
    static constexpr uint32_t kDrawMin = 0x1fc;
    static constexpr uint32_t kHeightTable = 0x20c;
    // mAnimCounter, s16 in the high half.
    static constexpr uint32_t kAnimCounter = 0x22c;
    static constexpr uint32_t kGridCells = 65;
    static constexpr uint32_t kHeights = kGridCells * kGridCells;
    // The grid follows the player; a larger step in one tick is a warp, not travel.
    static constexpr float kMaxStep = 800.0f;

    struct Seams {
        DrawInterpolation::ReadWords readWords;
        DrawInterpolation::WriteWords writeWords;
    };

    explicit SeaInterpolation(Seams seams);

    void onDrawPhaseBegin(uint64_t tick) override;
    void onDrawPhaseEnd() override;

    void onActorDraw(uint32_t /*actor*/) override {
    }

    std::string json() const;

  private:
    // The sea as a tick left it: grid corner, then the heights.
    struct Seen {
        uint64_t tick = 0;
        uint32_t heightTable = 0;
        std::vector<uint32_t> words;
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

    void blend(uint32_t packet, const Seen& previous, const Seen& current);
    void write(uint32_t address, Change change);

    DrawInterpolation::ReadWords m_readWords;
    DrawInterpolation::WriteWords m_writeWords;

    mutable std::mutex m_mutex;
    Seen m_seen;
    std::vector<Restore> m_restores;

    uint64_t m_blends = 0;
    uint64_t m_firstSeen = 0;
    uint64_t m_warps = 0;
    uint64_t m_counterHolds = 0;
    uint64_t m_unblendable = 0;
    uint64_t m_unreadable = 0;
    uint64_t m_writeFailures = 0;
    uint64_t m_restored = 0;
};

} // namespace wiiuport::title
