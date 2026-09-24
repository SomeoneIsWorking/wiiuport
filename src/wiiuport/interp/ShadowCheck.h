#pragma once

#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/frame/GuestMemorySnapshot.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace wiiuport::interp {

// Whether drawing an in-between frame changes any guest memory.
//
// Runs on the rendering thread while the frame gate holds, so the title's CPU
// is waiting on its swap and nothing else draws. Each round copies guest
// memory, draws one in-between frame through the shipping path, and lists the
// pages that changed; then copies it again and waits as long as that frame
// took, drawing nothing -- the control. The guest's audio and timer threads
// write memory in both windows, so a page is charged to the in-between frame
// only when it changed in every in-between window and in no control window.
//
// Every in-between window, because the frame is drawn the same way each
// round: a write of a value that changes repeats in each, and a write of one
// that does not already landed in the thousands of in-between frames drawn
// before the check, so it shows in none. A page the background touches
// rarely can still land only in some in-between windows; it is listed, not
// charged, and more rounds are what show it in a control window.
class ShadowCheck {
  public:
    using Regions = std::function<std::vector<frame::GuestMemorySnapshot::Region>()>;
    // Draws one in-between frame for the held frame; false when it drew none.
    using DrawInBetween = std::function<bool(const frame::FrameRecording&)>;
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;
    using Wait = std::function<void(Clock::duration)>;

    struct Result {
        uint32_t rounds{0};
        uint32_t inBetweensDrawn{0};
        uint64_t bytesCompared{0};
        // Distinct pages, ascending.
        std::vector<uint32_t> controlPages;
        std::vector<uint32_t> inBetweenPages;
        std::vector<uint32_t> inBetweenOnly;
        // For each of those, how many in-between windows it changed in. A
        // write the frame makes repeats round after round; a background
        // buffer rotating past lands in one window here and another there.
        std::vector<uint32_t> inBetweenOnlyWindows;
        // Why the check could not be made; empty when it was.
        std::string refusal;

        // The in-between-only pages that changed in every in-between window.
        size_t charged() const {
            return static_cast<size_t>(std::ranges::count(inBetweenOnlyWindows, rounds));
        }

        bool clean() const {
            return refusal.empty() && rounds > 0 && inBetweensDrawn == rounds && charged() == 0;
        }
    };

    ShadowCheck(Regions regions, DrawInBetween draw, Now now, Wait wait);

    Result run(const frame::FrameRecording& held, uint32_t rounds);

    static std::string toJson(const Result& result);

  private:
    Regions m_regions;
    DrawInBetween m_draw;
    Now m_now;
    Wait m_wait;
    frame::GuestMemorySnapshot m_snapshot;
};

} // namespace wiiuport::interp
