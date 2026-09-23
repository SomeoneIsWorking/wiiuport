// Plans a kept recordings snapshot again through the shipping planner and
// writes what it came to as one JSON object. tools/plan_replay.py builds and
// runs it; see there.

#include "wiiuport/frame/RecordingSnapshot.h"
#include "wiiuport/interp/PlanReplay.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

namespace {

using wiiuport::interp::ObjectPlanner;
using wiiuport::interp::PlanReplay;
using wiiuport::interp::ShaderKey;

std::string readAll(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(std::string("cannot read ") + path);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string outcomesJson(const ObjectPlanner::Outcomes& outcomes) {
    std::string body = "{";
    for (size_t outcome = 0; outcome < outcomes.size(); ++outcome) {
        body += outcome == 0 ? "\"" : ",\"";
        body += ObjectPlanner::outcomeName(static_cast<ObjectPlanner::Outcome>(outcome));
        body += "\":" + std::to_string(outcomes[outcome]);
    }
    return body + "}";
}

std::string hex(uint64_t value) {
    std::array<char, 17> text{};
    std::snprintf(text.data(), text.size(), "%016llx", static_cast<unsigned long long>(value));
    return text.data();
}

std::string shadersJson(const std::map<ShaderKey, PlanReplay::ShaderReport>& shaders) {
    std::string body = "[";
    for (const auto& [key, shader] : shaders) {
        body += body.size() == 1 ? "{" : ",{";
        body += "\"baseHash\":\"" + hex(key.baseHash) + "\"";
        body += ",\"auxHash\":\"" + hex(key.auxHash) + "\"";
        body += ",\"stageIndex\":" + std::to_string(key.stageIndex);
        body += ",\"outcomes\":" + outcomesJson(shader.outcomes);
        body += ",\"compared\":" + std::to_string(shader.compared);
        body += ",\"leftAtN\":" + std::to_string(shader.leftAtN) + "}";
    }
    return body + "]";
}

std::string reportJson(const PlanReplay::Report& report) {
    std::string body = "{\"frames\":" + std::to_string(report.frames);
    body += ",\"framesPlanned\":" + std::to_string(report.framesPlanned);
    body += ",\"outcomes\":" + outcomesJson(report.outcomes);
    body += ",\"partnersDerived\":" + std::to_string(report.partnersDerived);
    body += ",\"partnersSearched\":" + std::to_string(report.partnersSearched);
    body += ",\"partnersReidentified\":" + std::to_string(report.partnersReidentified);
    body += ",\"reidentifyAttempts\":" + std::to_string(report.reidentifyAttempts);
    body += ",\"partnerCandidates\":" + std::to_string(report.partnerCandidates);
    body += ",\"nearestCandidates\":" + std::to_string(report.nearestCandidates);
    body += ",\"shaders\":" + shadersJson(report.shaders);
    body += ",\"leftAtNByFrame\":[";
    for (size_t frame = 0; frame < report.leftAtNByFrame.size(); ++frame) {
        body += (frame == 0 ? "" : ",") + std::to_string(report.leftAtNByFrame[frame]);
    }
    body += "]";
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
