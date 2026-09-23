#include "wiiuport/interp/PlanReplay.h"

#include <algorithm>
#include <stdexcept>

namespace wiiuport::interp {

namespace {

// Where the product draws its in-between frame.
constexpr float kHalfway = 0.5f;

} // namespace

PlanReplay::Report PlanReplay::run(std::span<const frame::RecordingSnapshot::Frame> frames,
                                   uint32_t repeats) {
    if (repeats == 0) {
        throw std::invalid_argument("a replay repeated no times plans nothing");
    }
    Report report;
    report.planning = std::chrono::nanoseconds::max();
    for (uint32_t repeat = 0; repeat < repeats; ++repeat) {
        ObjectPlanner planner(kHalfway);
        Report counted;
        auto started = std::chrono::steady_clock::now();
        for (const frame::RecordingSnapshot::Frame& frame : frames) {
            for (const frame::RecordedUniformAssembly& assembly : frame.assemblies) {
                planner.add(assembly);
            }
            planner.endFrame();
            ++counted.frames;
            if (!planner.ready()) {
                continue;
            }
            ++counted.framesPlanned;
            const ObjectPlanner::Outcomes& planned = planner.latestOutcomes();
            for (size_t outcome = 0; outcome < planned.size(); ++outcome) {
                counted.outcomes[outcome] += planned[outcome];
            }
        }
        auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);
        counted.partnersDerived = planner.partnersDerived();
        counted.partnersSearched = planner.partnersSearched();
        counted.partnersReidentified = planner.partnersReidentified();
        counted.reidentifyAttempts = planner.reidentifyAttempts();
        counted.partnerCandidates = planner.partnerCandidates();
        counted.nearestCandidates = planner.nearestCandidates();
        counted.planning = std::min(report.planning, took);
        report = counted;
    }
    return report;
}

} // namespace wiiuport::interp
