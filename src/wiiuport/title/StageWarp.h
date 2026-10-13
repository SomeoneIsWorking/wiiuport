#pragma once

#include "wiiuport/title/DrawInterpolation.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wiiuport::title {

// Asks Wind Waker HD for another stage the way its own exits do.
//
// Writes what dComIfGp_setNextStage (0x0252012c) writes with its defaults: the next stage request
// in g_dComIfG_gameInfo.play (name, point, room, layer, then wipe and enable), the last speed and
// mode the player leaves with (mode from the player's shields) and the restart point. The play
// scene changes stage once it sees the request enabled. A request already pending is refused, as
// dStage_nextStage_c::set refuses it.
class StageWarp {
  public:
    // g_dComIfG_gameInfo.play and its dStage_nextStage_c.
    static constexpr uint32_t kPlay = 0x1046f0b0;
    static constexpr uint32_t kNextStage = kPlay + 0x5140;
    // Name (8 bytes), then point (s16), room (s8) and layer (s8), then enable and wipe (s8).
    static constexpr uint32_t kNameWords = 2;
    static constexpr uint32_t kPlaceWord = kNextStage + 8;
    static constexpr uint32_t kEnableWord = kNextStage + 0xc;
    // The player (daPy_lk_c): its flags (dragon shield 0x1, mirror shield 0x8000) and the
    // shield's tinkle state (s16).
    static constexpr uint32_t kPlayer = kPlay + 0x5b34;
    static constexpr uint32_t kPlayerFlags = 0x3bc;
    static constexpr uint32_t kTinkleShield = 0x699e;
    // Holds the save info: restart point (s16, high half), last speed and last mode.
    static constexpr uint32_t kSaveInfo = 0x101f84dc;
    static constexpr uint32_t kRestartPoint = 0x115c;
    static constexpr uint32_t kLastSpeed = 0x1170;
    static constexpr uint32_t kLastMode = 0x1174;
    static constexpr size_t kMaxName = 7;

    struct Request {
        std::string stage;
        int16_t point = 0;
        int8_t room = 0;
        int8_t layer = -1;
    };

    struct Seams {
        DrawInterpolation::ReadWords readWords;
        DrawInterpolation::WriteWords writeWords;
    };

    explicit StageWarp(Seams seams);

    // `stage=<name>&point=<n>&room=<n>[&layer=<n>]`, or a refusal naming what is wrong.
    static std::optional<Request> parse(std::string_view query, std::string& refusal);

    // Makes the request; a refusal names why it was not made.
    bool warp(const Request& request, std::string& refusal);

  private:
    DrawInterpolation::ReadWords m_readWords;
    DrawInterpolation::WriteWords m_writeWords;
};

} // namespace wiiuport::title
