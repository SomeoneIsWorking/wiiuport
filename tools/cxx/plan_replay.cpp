// Plans a kept recordings snapshot again through the shipping planner and
// writes what it came to as one JSON object. tools/plan_replay.py builds and
// runs it; see there.

#include "wiiuport/frame/RecordingSnapshot.h"
#include "wiiuport/interp/PlanReplay.h"

#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using wiiuport::interp::ObjectPlanner;
using wiiuport::interp::PlanReplay;

std::string readAll(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(std::string("cannot read ") + path);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string reportJson(const PlanReplay::Report& report) {
    std::string body = "{\"frames\":" + std::to_string(report.frames);
    body += ",\"framesPlanned\":" + std::to_string(report.framesPlanned);
    body += ",\"outcomes\":{";
    for (size_t outcome = 0; outcome < report.outcomes.size(); ++outcome) {
        body += outcome == 0 ? "\"" : ",\"";
        body += ObjectPlanner::outcomeName(static_cast<ObjectPlanner::Outcome>(outcome));
        body += "\":" + std::to_string(report.outcomes[outcome]);
    }
    body += "},\"partnersDerived\":" + std::to_string(report.partnersDerived);
    body += ",\"partnersSearched\":" + std::to_string(report.partnersSearched);
    body += ",\"partnersReidentified\":" + std::to_string(report.partnersReidentified);
    body += ",\"reidentifyAttempts\":" + std::to_string(report.reidentifyAttempts);
    body += ",\"partnerCandidates\":" + std::to_string(report.partnerCandidates);
    body += ",\"nearestCandidates\":" + std::to_string(report.nearestCandidates);
    body += ",\"planningNanoseconds\":" + std::to_string(report.planning.count()) + "}";
    return body;
}

int replay(int argc, char** argv) {
    if (argc != 3) {
        std::fputs("usage: wiiuport_plan_replay <recordings.bin> <repeats>\n", stderr);
        return 2;
    }
    auto frames = wiiuport::frame::RecordingSnapshot::parse(readAll(argv[1]));
    PlanReplay::Report report = PlanReplay::run(frames, std::stoul(argv[2]));
    std::puts(reportJson(report).c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return replay(argc, argv);
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "wiiuport_plan_replay: %s\n", failure.what());
        return 1;
    }
}
