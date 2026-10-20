#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"
#include "wiiuport/title/DrawInterpolation.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's screen fade in the in-between paint.
//
// The fade manager's fader steps its alpha once a tick in the frame logic (0x02728a74), and the
// render pass draws it from the fader's colour at paint time (0x02728c54), so both paints of a
// tick showed the same alpha. In the paint drawn from a tick's draw phase the alpha is set to the
// midpoint of what the previous paint drew and what it is now, and put back when that paint ends.
class FadeInterpolation final : public DrawInterpolation::MidPaintListener {
  public:
    // The fade manager's render callback (manager in r3).
    static constexpr uint32_t kFaderDraw = 0x02728c54;
    static constexpr uint32_t kFaderDrawFirst = 0x7c0802a6; // mflr r0
    static constexpr size_t kManagerRegister = 3;
    // The manager's fader; the fader's status, then its colour (RGBA8, alpha in the low byte).
    static constexpr uint32_t kFader = 0x440;
    static constexpr uint32_t kStatus = 0x0;
    static constexpr uint32_t kColor = 0x8;
    // Faded in: the draw skips the fader whatever its alpha holds.
    static constexpr uint32_t kClear = 1;
    static constexpr uint32_t kAlphaMask = 0xff;

    struct Seams {
        DrawInterpolation::Register registerProbe;
        DrawInterpolation::ReadWords readWords;
        DrawInterpolation::WriteWords writeWords;
    };

    explicit FadeInterpolation(Seams seams);

    // Registers with the fork, before the title is linked.
    void install();

    // On the display thread.
    void onFaderDraw(uint32_t manager);
    void onMidPaintBegin(uint64_t tick) override;
    void onMidPaintEnd() override;

    std::string json() const;

  private:
    class Entry final : public GuestCallProbes::Probe {
      public:
        explicit Entry(FadeInterpolation& owner) : m_owner(owner) {
        }

        void OnInstall(GuestCallProbes::Installation installation) override;
        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;
        std::optional<GuestCallProbes::Installation> installation;

      private:
        FadeInterpolation& m_owner;
    };

    // The alpha a paint showed the fader at.
    struct Seen {
        uint64_t tick = 0;
        uint32_t fader = 0;
        uint32_t alpha = 0;
    };

    struct Restore {
        uint32_t address = 0;
        uint32_t color = 0;
    };

    DrawInterpolation::Register m_register;
    DrawInterpolation::ReadWords m_readWords;
    DrawInterpolation::WriteWords m_writeWords;
    Entry m_faderDraw{*this};

    mutable std::mutex m_mutex;
    bool m_inMidPaint = false;
    // The tick the last mid paint was drawn from; the paint after it draws that tick as it is.
    uint64_t m_tick = 0;
    Seen m_seen;
    std::optional<Restore> m_restore;

    uint64_t m_draws = 0;
    uint64_t m_blends = 0;
    uint64_t m_firstSeen = 0;
    uint64_t m_unreadable = 0;
    uint64_t m_writeFailures = 0;
    uint64_t m_restored = 0;
};

} // namespace wiiuport::title
