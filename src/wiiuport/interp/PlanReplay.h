#pragma once

#include "wiiuport/frame/RecordingSnapshot.h"
#include "wiiuport/interp/ObjectPlanner.h"

#include <chrono>
#include <cstdint>
#include <span>

namespace wiiuport::interp {

// Plans a kept snapshot's frames again, offline, through the planner the
// product runs.
//
// A run on the title lands somewhere different each time, and what planning
// costs differs several times over between scenes, so two versions of the
// planner can only be compared on the same frames. The snapshot holds them;
// this feeds them draw by draw and frame by frame as ObjectBlend does, on one
// thread, and times the whole of it.
class PlanReplay {
  public:
    struct Report {
        // Frames fed, and those planned against two whole frames before them.
        uint64_t frames{0};
        uint64_t framesPlanned{0};
        // What the planned frames' objects came to, summed.
        ObjectPlanner::Outcomes outcomes{};
        uint64_t partnersDerived{0};
        uint64_t partnersSearched{0};
        uint64_t partnersReidentified{0};
        uint64_t reidentifyAttempts{0};
        uint64_t partnerCandidates{0};
        uint64_t nearestCandidates{0};
        // The fastest of the repeats: every draw added and every frame ended.
        std::chrono::nanoseconds planning{0};
    };

    // Plans the frames `repeats` times over, each time with a fresh planner;
    // the counts are the same every time, the time is the least taken.
    static Report run(std::span<const frame::RecordingSnapshot::Frame> frames, uint32_t repeats);
};

} // namespace wiiuport::interp
