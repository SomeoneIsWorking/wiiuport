#include "check.h"
#include "suites.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/interp/ObjectBlend.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

using wiiuport::frame::FrameRecording;
using wiiuport::frame::RecordedUniformAssembly;
using wiiuport::interp::ObjectBlend;
using Outcome = wiiuport::interp::ObjectBlend::Outcome;

namespace {

// Half way between the title's two frames, as the product blends.
constexpr float kHalfway = 0.5f;
constexpr uint64_t kActorShader = 0xdddd;
// A second pass drawing the same actor, as its outline.
constexpr uint64_t kOutline = 0xeeee;
// The two blocks one actor alternates between, as the title double-buffers
// them: A on even frames, B on odd.
constexpr uint32_t kBlockA = 0xf4001000;
constexpr uint32_t kBlockB = 0xf4081000;
constexpr uint32_t kOtherA = 0xf4002000;
constexpr uint32_t kOtherB = 0xf4082000;

struct Draw {
    uint32_t block;
    std::vector<float> values;
    uint64_t shader{kActorShader};
};

FrameRecording frameOf(const std::vector<Draw>& draws) {
    FrameRecording frame;
    for (const Draw& draw : draws) {
        RecordedUniformAssembly assembly;
        assembly.shaderBaseHash = draw.shader;
        assembly.blockSources = {1, draw.block};
        assembly.data = draw.values;
        frame.addUniformAssembly(assembly);
    }
    return frame;
}

// One replayed draw as the renderer hands it over: its own buffer, sourced
// from the block the guest's frame N drew it from.
struct ReplayedDraw {
    std::array<uint32_t, 2> sources;
    std::vector<float> values;
    LatteFrameHooks::UniformAssembly assembly{};

    explicit ReplayedDraw(const Draw& draw) : sources{1, draw.block}, values(draw.values) {
        assembly.shaderBaseHash = draw.shader;
        assembly.data = values.data();
        assembly.sizeInBytes = static_cast<uint32_t>(values.size() * sizeof(float));
        assembly.blockAddresses = sources.data();
        assembly.blockAddressCount = 1;
        assembly.fromRuntime = true;
    }
};

// Replays frame N as the product does -- every draw, in the order recorded --
// and returns what each draw uploaded.
std::vector<std::vector<float>> replay(ObjectBlend& blend, const std::vector<Draw>& frame) {
    std::vector<std::vector<float>> uploaded;
    for (const Draw& draw : frame) {
        ReplayedDraw replayed(draw);
        blend.apply(replayed.assembly);
        uploaded.push_back(replayed.values);
    }
    return uploaded;
}

// One guest frame as the recorder hands it over: each draw as it is recorded,
// then the frame's end.
void record(ObjectBlend& blend, const std::vector<Draw>& draws) {
    FrameRecording frame = frameOf(draws);
    for (const RecordedUniformAssembly& assembly : frame.uniformAssemblies()) {
        blend.onAssemblyRecorded(assembly);
    }
    blend.onFrameRecorded(frame);
}

// Three frames fed to a planning blend and armed on the last.
void armAfter(ObjectBlend& blend, const std::vector<Draw>& twoBack,
              const std::vector<Draw>& oneBack, const std::vector<Draw>& latest) {
    blend.setPlanning(true);
    record(blend, twoBack);
    record(blend, oneBack);
    record(blend, latest);
    blend.armOnce();
}

// One actor walking one unit a frame, and a second standing still.
const std::vector<Draw> kWalkTwoBack{{kBlockA, {0.0f, 7.0f}}, {kOtherA, {5.0f, 5.0f}}};
const std::vector<Draw> kWalkOneBack{{kBlockB, {1.0f, 7.0f}}, {kOtherB, {5.0f, 5.0f}}};
const std::vector<Draw> kWalkLatest{{kBlockA, {2.0f, 7.0f}}, {kOtherA, {5.0f, 5.0f}}};

void aMovingObjectIsDrawnHalfWayBetweenItsFrames() {
    ObjectBlend blend{kHalfway};
    armAfter(blend, kWalkTwoBack, kWalkOneBack, kWalkLatest);
    auto uploaded = replay(blend, kWalkLatest);
    check::equal(uploaded[0][0], 1.5f, "the walker half way between frames N-1 and N");
    check::equal(uploaded[0][1], 7.0f, "and what did not move stays put");
    check::equal(blend.objects(Outcome::Blended), uint64_t{1}, "one object blended");
    check::equal(blend.drawsWritten(), uint64_t{1}, "and its draw written");
}

void aStillObjectIsDrawnExactlyAsTheTitleDrewIt() {
    ObjectBlend blend{kHalfway};
    armAfter(blend, kWalkTwoBack, kWalkOneBack, kWalkLatest);
    auto uploaded = replay(blend, kWalkLatest);
    check::isTrue(uploaded[1] == std::vector<float>{5.0f, 5.0f}, "bit for bit");
    check::equal(blend.objects(Outcome::Held), uint64_t{1}, "and counted as held");
}

void anAddressReusedByAnotherObjectIsNotBlended() {
    // Block A now holds something a hundred units away: not the actor that
    // stood there two frames ago, and nothing in N-1 passed through between.
    std::vector<Draw> latest{{kBlockA, {100.0f, -3.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, 7.0f}}}, {{kBlockB, {1.0f, 7.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(uploaded[0][0], 100.0f, "it is drawn where the title put it");
    check::equal(blend.objects(Outcome::Unverified), uint64_t{1}, "and counted as unverified");
}

void aNewObjectIsDrawnAsDrawnAndCounted() {
    std::vector<Draw> latest{{kBlockA, {2.0f, 7.0f}}, {0xf4003000, {9.0f, 9.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, 7.0f}}}, {{kBlockB, {1.0f, 7.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(uploaded[1][0], 9.0f, "an object with no frame N-2 is not written");
    check::equal(blend.objects(Outcome::Unmatched), uint64_t{1}, "and is counted unmatched");
    check::equal(uploaded[0][0], 1.5f, "while the object beside it still is");
}

void aPartnerFoundOnceIsCarriedAndRechecked() {
    ObjectBlend blend{kHalfway};
    armAfter(blend, kWalkTwoBack, kWalkOneBack, kWalkLatest);
    uint64_t searched = blend.planner().partnersSearched();
    record(blend, {{kBlockB, {3.0f, 7.0f}}, {kOtherB, {5.0f, 5.0f}}});
    blend.armOnce();
    check::equal(blend.planner().partnersSearched(), searched,
                 "the next frame's partner is the one already found, not searched for again");
    check::equal(blend.planner().partnersDerived(), uint64_t{1},
                 "but derived from the pair of blocks");
    check::equal(blend.objects(Outcome::Blended), uint64_t{2}, "and it is blended again");
}

void oneObjectsBlocksPairEveryShaderDrawingIt() {
    // The actor's outline pass reads the same blocks as its body.
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, 7.0f}}, {kBlockA, {0.0f, 1.0f}, kOutline}},
             {{kBlockB, {1.0f, 7.0f}}, {kBlockB, {1.0f, 1.0f}, kOutline}},
             {{kBlockA, {2.0f, 7.0f}}, {kBlockA, {2.0f, 1.0f}, kOutline}});
    check::equal(blend.planner().partnersSearched(), uint64_t{1},
                 "the body's search pairs the blocks");
    check::equal(blend.planner().partnersDerived(), uint64_t{1},
                 "and the outline's partner follows");
    check::equal(blend.objects(Outcome::Blended), uint64_t{2}, "both are blended");
}

void aShapeThatMovesTheSameButStandsElsewhereIsNotAPartner() {
    // Moving exactly as the actor would, but at another height: a different
    // object, told apart by the value that did not move.
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, 7.0f}}}, {{kBlockB, {1.0f, 40.0f}}},
             {{kBlockA, {2.0f, 7.0f}}});
    check::equal(blend.objects(Outcome::Unverified), uint64_t{1},
                 "a value the object held that its partner does not have fails the match");
}

void aFailedSearchWaitsBeforeItIsRunAgain() {
    // A sprite that flips between two poses: frame N-1 is one of its ends,
    // never the middle, so no partner is ever found.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    for (int frame = 0; frame < 3; ++frame) {
        record(blend, {{frame % 2 == 0 ? kBlockA : kBlockB, {frame % 4 < 2 ? 0.0f : 1.0f}}});
    }
    blend.armOnce();
    check::equal(blend.planner().partnersSearched(), uint64_t{1}, "the first frame searches");
    uint64_t armed = 1;
    for (int frame = 3; armed < 1 + wiiuport::interp::ObjectPlanner::kSearchRetryInterval;
         ++frame) {
        record(blend, {{frame % 2 == 0 ? kBlockA : kBlockB, {frame % 4 < 2 ? 0.0f : 1.0f}}});
        blend.armOnce();
        ++armed;
    }
    check::isTrue(blend.planner().searchesDeferred() > 0, "later frames of the same object wait");
    check::isTrue(blend.planner().partnersSearched() < armed, "so it is not searched every frame");
    check::equal(blend.objects(Outcome::Blended), uint64_t{0}, "and it is never blended");
}

void aValueThatIsNotANumberIsTakenFromTheLaterFrame() {
    // Small integers stored in a float's bits read as denormals.
    float three = 4.2e-45f;
    float five = 7.0e-45f;
    std::vector<Draw> latest{{kBlockA, {2.0f, five}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, three}}}, {{kBlockB, {1.0f, three}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(uploaded[0][0], 1.5f, "the number is blended");
    check::equal(uploaded[0][1], five, "the integer is the later frame's");
    check::equal(blend.planner().valuesNotBlended(), uint64_t{1}, "and counted");
}

void twoDrawsFromOneBlockAreTwoObjects() {
    std::vector<Draw> latest{{kBlockA, {2.0f}}, {kBlockA, {14.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f}}, {kBlockA, {10.0f}}},
             {{kBlockB, {1.0f}}, {kBlockB, {12.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(uploaded[0][0], 1.5f, "the first draw is blended with the first");
    check::equal(uploaded[1][0], 13.0f, "and the second with the second");
}

void aReplayOutOfStepWithTheRecordingStopsWriting() {
    ObjectBlend blend{kHalfway};
    armAfter(blend, kWalkTwoBack, kWalkOneBack, kWalkLatest);
    // The replay's first draw is not the recording's first.
    auto uploaded = replay(blend, {kWalkLatest[1], kWalkLatest[0]});
    check::equal(blend.replaysDiverged(), uint64_t{1}, "the divergence is counted");
    check::equal(uploaded[1][0], 2.0f, "and nothing after it is written, even a planned blend");
    check::equal(blend.drawsWritten(), uint64_t{0}, "so no draw was written at all");
}

void nothingIsWrittenWhenNotArmed() {
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    for (const auto* frame : {&kWalkTwoBack, &kWalkOneBack, &kWalkLatest}) {
        record(blend, *frame);
    }
    replay(blend, kWalkLatest);
    check::equal(blend.drawsWritten(), uint64_t{0}, "an unarmed blend writes nothing");
    ObjectBlend young{kHalfway};
    young.setPlanning(true);
    record(young, {{kBlockA, {0.0f}}});
    check::isTrue(!young.armOnce(), "and fewer than three frames refuse to arm");
}

// Draws of the actor's shader in N-1 that are nowhere near its path, spread
// widely at `position`, each from its own block.
std::vector<Draw> withDecoys(std::vector<Draw> draws, size_t position) {
    for (uint32_t decoy = 0; decoy < 100; ++decoy) {
        std::vector<float> values{50.0f, 50.0f, 50.0f};
        values[position] = 1000.0f + static_cast<float>(decoy) * 10.0f;
        draws.push_back({0xf4100000 + (decoy * 0x100), values});
    }
    return draws;
}

void aPartnerIsFoundAmongManyDrawsOfItsShader() {
    std::vector<Draw> latest{{kBlockA, {2.0f, 7.0f, 3.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, 7.0f, 3.0f}}}, withDecoys({{kBlockB, {1.0f, 7.0f, 3.0f}}}, 0),
             latest);
    auto uploaded = replay(blend, latest);
    check::equal(uploaded[0][0], 1.5f, "the partner is found among a hundred others");
    check::equal(blend.objects(Outcome::Blended), uint64_t{1}, "and the object blended");
}

void aPartnerWithNoNumberWhereItsShaderIsOrderedIsStillFound() {
    // The decoys spread widest at the third value, where the partner holds
    // an integer: it cannot be ordered there, and must not be lost for it.
    float integer = 4.2e-45f;
    std::vector<Draw> latest{{kBlockA, {2.0f, 7.0f, 3.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, 7.0f, 3.0f}}},
             withDecoys({{kBlockB, {1.0f, 7.0f, integer}}}, 2), latest);
    auto uploaded = replay(blend, latest);
    check::equal(uploaded[0][0], 1.5f, "the unordered partner is found");
}

constexpr uint32_t kActors = 500;

void aFrameOfManyDrawsIsPlannedWhileItIsDrawn() {
    // Far more draws than the planning thread is woken for, so it plans
    // while later ones are still being handed over.
    std::array<std::vector<Draw>, 3> frames;
    for (uint32_t actor = 0; actor < kActors; ++actor) {
        auto x = static_cast<float>(actor) * 100.0f;
        uint32_t even = 0xf4200000 + (actor * 0x100);
        uint32_t odd = 0xf4600000 + (actor * 0x100);
        frames[0].push_back({even, {x, 7.0f}});
        frames[1].push_back({odd, {x + 1.0f, 7.0f}});
        frames[2].push_back({even, {x + 2.0f, 7.0f}});
    }
    ObjectBlend blend{kHalfway};
    armAfter(blend, frames[0], frames[1], frames[2]);
    auto uploaded = replay(blend, frames[2]);
    check::equal(blend.objects(Outcome::Blended), uint64_t{kActors}, "every actor is blended");
    check::equal(uploaded[kActors - 1][0], (static_cast<float>(kActors - 1) * 100.0f) + 1.5f,
                 "the last one handed over too");
}

void turningPlanningOffForgetsTheFramesItHeld() {
    ObjectBlend blend{kHalfway};
    armAfter(blend, kWalkTwoBack, kWalkOneBack, kWalkLatest);
    blend.setPlanning(false);
    record(blend, kWalkTwoBack);
    blend.setPlanning(true);
    record(blend, kWalkOneBack);
    record(blend, kWalkLatest);
    check::isTrue(!blend.armOnce(), "frames from before it was off are not blended across");
    record(blend, kWalkTwoBack);
    check::isTrue(blend.armOnce(), "three new frames are");
}

} // namespace

namespace wiiuport::tests {

void runObjectBlendTests() {
    aMovingObjectIsDrawnHalfWayBetweenItsFrames();
    aStillObjectIsDrawnExactlyAsTheTitleDrewIt();
    anAddressReusedByAnotherObjectIsNotBlended();
    aNewObjectIsDrawnAsDrawnAndCounted();
    aPartnerFoundOnceIsCarriedAndRechecked();
    oneObjectsBlocksPairEveryShaderDrawingIt();
    aShapeThatMovesTheSameButStandsElsewhereIsNotAPartner();
    aFailedSearchWaitsBeforeItIsRunAgain();
    aValueThatIsNotANumberIsTakenFromTheLaterFrame();
    twoDrawsFromOneBlockAreTwoObjects();
    aReplayOutOfStepWithTheRecordingStopsWriting();
    nothingIsWrittenWhenNotArmed();
    aPartnerIsFoundAmongManyDrawsOfItsShader();
    aPartnerWithNoNumberWhereItsShaderIsOrderedIsStillFound();
    aFrameOfManyDrawsIsPlannedWhileItIsDrawn();
    turningPlanningOffForgetsTheFramesItHeld();
}

} // namespace wiiuport::tests
