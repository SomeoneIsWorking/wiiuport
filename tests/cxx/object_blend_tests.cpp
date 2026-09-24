#include "check.h"
#include "suites.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/interp/ObjectBlend.h"
#include "wiiuport/interp/PlanReplay.h"
#include "wiiuport/interp/ReplayBlend.h"
#include "wiiuport/interp/SharedTransforms.h"
#include "wiiuport/interp/TransformSubstitution.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

using wiiuport::frame::FrameRecording;
using wiiuport::frame::RecordedUniformAssembly;
using wiiuport::interp::ObjectBlend;
using Outcome = wiiuport::interp::ObjectBlend::Outcome;
using wiiuport::interp::ObjectPlanner;
using wiiuport::interp::ShaderKey;
using wiiuport::interp::SharedTransforms;

namespace {

// Half way between the title's two frames, as the product blends.
constexpr float kHalfway = 0.5f;
constexpr uint64_t kActorShader = 0xdddd;
constexpr uint64_t kPierShader = 0xeeee;
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
    // A second block at an address the title allocated for this frame alone.
    uint32_t freshBlock{0};
    uint32_t stage{0};
    bool writesColour{true};

    std::vector<uint32_t> sources() const {
        if (freshBlock == 0) {
            return {1, block};
        }
        return {1, block, 2, freshBlock};
    }
};

FrameRecording frameOf(const std::vector<Draw>& draws) {
    FrameRecording frame;
    for (const Draw& draw : draws) {
        RecordedUniformAssembly assembly;
        assembly.shaderBaseHash = draw.shader;
        assembly.stageIndex = draw.stage;
        assembly.writesColour = draw.writesColour;
        assembly.blockSources = draw.sources();
        assembly.data = draw.values;
        frame.addUniformAssembly(assembly);
    }
    return frame;
}

// One replayed draw as the renderer hands it over: its own buffer, sourced
// from the block the guest's frame N drew it from.
struct ReplayedDraw {
    std::vector<uint32_t> sources;
    std::vector<float> values;
    LatteFrameHooks::UniformAssembly assembly{};

    explicit ReplayedDraw(const Draw& draw) : sources(draw.sources()), values(draw.values) {
        assembly.shaderBaseHash = draw.shader;
        assembly.stageIndex = draw.stage;
        assembly.writesColour = draw.writesColour;
        assembly.data = values.data();
        assembly.sizeInBytes = static_cast<uint32_t>(values.size() * sizeof(float));
        assembly.blockAddresses = sources.data();
        assembly.blockAddressCount = static_cast<uint32_t>(sources.size() / 2);
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
    check::equal(blend.framesEnded(), uint64_t{3}, "each frame's end counted with its wait");
    check::isTrue(blend.planningBusy().count() > 0, "and the planning thread's time counted");
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

void aValueTheObjectHeldIsDrawnAsItsOwn() {
    // N-1's draw passes through the middle of the actor's move but holds 40
    // where the actor held 7: another object, or the actor's value flipping
    // with the title's double buffering -- three frames cannot tell. Either
    // way only what moved is taken from it, and that is where the actor
    // passed through; what the actor held is drawn as the actor's.
    std::vector<Draw> latest{{kBlockA, {2.0f, 7.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, 7.0f}}}, {{kBlockB, {1.0f, 40.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(uploaded[0][0], 1.5f, "what moved is blended");
    check::equal(uploaded[0][1], 7.0f, "and what it held is its own, not the other draw's");
}

void aTileWhoseBlocksPassedToAnotherIsFoundByItsValues() {
    // Two sea tiles standing still under a value every tile moves in, as the
    // camera does. The grid shifts between N-2 and N, and each tile's blocks
    // pass to the other: by address, each is a tile a hundred units away.
    ObjectBlend blend{kHalfway};
    std::vector<Draw> latest{{kBlockA, {100.0f, 2.0f}}, {kOtherA, {0.0f, 2.0f}}};
    armAfter(blend, {{kBlockA, {0.0f, 1.0f}}, {kOtherA, {100.0f, 1.0f}}},
             {{kBlockB, {0.0f, 1.5f}}, {kOtherB, {100.0f, 1.5f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::isTrue(uploaded[0] == std::vector<float>{100.0f, 1.75f},
                  "the tile found by its values is blended with its own partner");
    check::isTrue(uploaded[1] == std::vector<float>{0.0f, 1.75f}, "and so is the other");
    check::equal(blend.objects(Outcome::Blended), uint64_t{2}, "both are blended");
    check::equal(blend.planner().partnersReidentified(), uint64_t{2},
                 "each found in N-2 by its values, not its blocks");
}

void anObjectDrawnFromBlocksNewToItIsFoundByItsValues() {
    // The actor walks on, but frame N draws it from a block no frame before
    // sourced; and a second object, as new, stands where nothing stood.
    ObjectBlend blend{kHalfway};
    std::vector<Draw> latest{{0xf4005000, {2.0f, 7.0f}}, {0xf4006000, {50.0f, -9.0f}}};
    armAfter(blend, {{kBlockA, {0.0f, 7.0f}}}, {{kBlockB, {1.0f, 7.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(uploaded[0][0], 1.5f, "the walker is blended from where its values put it");
    check::equal(uploaded[1][0], 50.0f, "while the new object nearest nothing is drawn as is");
    check::equal(blend.objects(Outcome::Blended), uint64_t{1}, "one blended");
    check::equal(blend.objects(Outcome::Unmatched), uint64_t{1}, "and one unmatched");
}

void anObjectFoundStandingStillByItsValuesIsHeld() {
    std::vector<Draw> latest{{0xf4005000, {5.0f, 5.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kOtherA, {5.0f, 5.0f}}}, {{kOtherB, {5.0f, 5.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::isTrue(uploaded[0] == std::vector<float>{5.0f, 5.0f}, "drawn bit for bit");
    check::equal(blend.objects(Outcome::Held), uint64_t{1}, "and counted as held");
}

// Where an actor stands frame by frame: a jump no search can follow, then a
// walk of one unit a frame.
constexpr std::array<float, 5> kJumpThenWalk{100.0f, 90.0f, 0.0f, 1.0f, 2.0f};

void aFailedSearchByValuesDoesNotDelayTheSearchByBlocks() {
    // An actor with a block allocated afresh every frame jumps between the
    // first two frames planned, so no search finds it; it walks after that.
    // Its key is the same once it has a frame two back, and the search by
    // blocks has not failed for it, only the search by values.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    std::vector<Draw> latest;
    for (uint32_t frame = 0; frame < kJumpThenWalk.size(); ++frame) {
        latest = {{frame % 2 == 0 ? kBlockA : kBlockB,
                   {kJumpThenWalk[frame], 7.0f},
                   kActorShader,
                   0xf4900000 + (frame * 0x100)}};
        record(blend, latest);
    }
    blend.armOnce();
    auto uploaded = replay(blend, latest);
    check::equal(uploaded[0][0], 1.5f, "it is blended once its blocks say who it is");
}

void aFrameWithNoDrawsStillLeavesTheOneBeforeSearchable() {
    // Nothing draws in the frame after the actor's, so no draw of that frame
    // indexes the actor's for searching; its end has to. The actor then comes
    // back from a block the frame two back never sourced, and is looked for
    // there by its values.
    wiiuport::interp::ObjectPlanner planner{kHalfway};
    auto feed = [&planner](const std::vector<Draw>& draws) {
        FrameRecording frame = frameOf(draws);
        for (const RecordedUniformAssembly& assembly : frame.uniformAssemblies()) {
            planner.add(assembly);
        }
        planner.endFrame();
    };
    feed({{kBlockA, {0.0f, 7.0f}}});
    feed({});
    feed({{kBlockB, {2.0f, 7.0f}}});
    check::equal(planner.reidentifyAttempts(), uint64_t{1}, "the actor is looked for by values");
    check::equal(planner.nearestCandidates(), uint64_t{1}, "among the one draw before the gap");
}

void aKeptSnapshotIsPlannedAgainAsTheProductPlansIt() {
    std::vector<wiiuport::frame::RecordingSnapshot::Frame> frames;
    for (const std::vector<Draw>* draws : {&kWalkTwoBack, &kWalkOneBack, &kWalkLatest}) {
        wiiuport::frame::FrameRecording frame = frameOf(*draws);
        frames.push_back(
            {true, {frame.uniformAssemblies().begin(), frame.uniformAssemblies().end()}});
    }
    auto report = wiiuport::interp::PlanReplay::run(frames, 3);
    check::equal(report.frames, uint64_t{3}, "every frame fed");
    check::equal(report.framesPlanned, uint64_t{1}, "the last planned against the two before");
    check::equal(report.outcomes[static_cast<size_t>(Outcome::Blended)], uint64_t{1},
                 "the walker blended");
    check::equal(report.outcomes[static_cast<size_t>(Outcome::Held)], uint64_t{1},
                 "and the stander held");
    check::equal(report.partnersSearched, uint64_t{1}, "counted once, not once a repeat");
    check::isTrue(report.leftAtNByFrame == std::vector<uint64_t>{0},
                  "and none left at N in the one frame planned");
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
    check::isTrue(blend.planner().reidentifyAttempts() < armed,
                  "neither by its blocks nor by its values");
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

void aBlendIsTakenFromItsPartnerNotAssumedSteady() {
    // Slowing down: two thirds of the way by N-1. Its midpoint lands within
    // the tolerance, and half way from N-1 to N is 2.5, not the 2.25 that
    // steady motion from N-2 would put it at.
    std::vector<Draw> latest{{kBlockA, {3.0f, 7.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, 7.0f}}}, {{kBlockB, {2.0f, 7.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Blended), uint64_t{1}, "it is blended");
    check::equal(uploaded[0][0], 2.5f, "half way between its N-1 and N");
}

void aBlockAllocatedAfreshEveryFrameDoesNotHideItsObject() {
    // Its own blocks alternate A and B; a third block is new every frame.
    // Keyed by that address, it would never be seen again. The first frames
    // are keyed with nothing two back to tell fresh blocks by.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    std::vector<std::vector<Draw>> frames;
    for (uint32_t frame = 0; frame < 5; ++frame) {
        uint32_t own = frame % 2 == 0 ? kBlockA : kBlockB;
        frames.push_back(
            {{own, {static_cast<float>(frame), 7.0f}, kActorShader, 0xf4900000 + (frame * 0x100)}});
        record(blend, frames.back());
    }
    blend.armOnce();
    auto uploaded = replay(blend, frames.back());
    check::equal(blend.objects(Outcome::Blended), uint64_t{1}, "it is blended");
    check::equal(uploaded[0][0], 3.5f, "half way between its N-1 and N");
    check::equal(blend.replaysDiverged(), uint64_t{0},
                 "and its replay, at the fresh block's real address, is in step");
}

void aValueFlippingEveryFrameIsNotAveraged() {
    // The second value is a flag the title flips each frame; the first moves.
    std::vector<Draw> latest{{kBlockA, {20.0f, 1.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, 1.0f}}}, {{kBlockB, {10.0f, 0.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(uploaded[0][0], 15.0f, "what moves is blended");
    check::equal(uploaded[0][1], 1.0f, "what flips is drawn as the title drew it");
    check::equal(blend.planner().valuesAlternating(), uint64_t{1}, "and counted");
}

void aFlippingValueFarLargerThanTheMoveDoesNotHideThePartner() {
    // Packed data the title writes every other frame, read as floats: 2^97
    // at N-2 and N, nothing at N-1. Over it the object's move of twenty
    // units is lost unless only the values it moved in are measured.
    float packed = std::ldexp(1.0f, 97);
    std::vector<Draw> latest{{kBlockA, {20.0f, packed}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, packed}}}, {{kBlockB, {10.0f, 0.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Blended), uint64_t{1}, "its partner is found");
    check::equal(uploaded[0][0], 15.0f, "what moves is blended");
    check::equal(uploaded[0][1], packed, "and the packed data drawn as the title wrote it");
}

void aValueEveryDrawHoldsDoesNotDecideThePartner() {
    // The second value is the frame's -- both objects are handed it -- and it
    // sways unevenly with the camera: 100, 90, 102. Over it neither object's
    // midpoint lands anywhere; over their own values both do.
    std::vector<Draw> latest{{kBlockA, {2.0f, 102.0f}}, {kOtherA, {12.0f, 102.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, 100.0f}}, {kOtherA, {10.0f, 100.0f}}},
             {{kBlockB, {1.0f, 90.0f}}, {kOtherB, {11.0f, 90.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Blended), uint64_t{2}, "both are blended");
    check::equal(uploaded[0][0], 1.5f, "each by its own move");
    check::equal(uploaded[1][0], 11.5f, "the other too");
    check::equal(uploaded[0][1], 96.0f, "and the frame's value half way from N-1 to N");
}

void drawsTheFrameAloneMovesAreBlendedWithAnyCandidate() {
    // Every draw of the shader holds the same values, which only the frame
    // moves: whichever is taken as the partner, the same is drawn.
    std::vector<Draw> latest{{kBlockA, {102.0f}}, {kOtherA, {102.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {100.0f}}, {kOtherA, {100.0f}}},
             {{kBlockB, {90.0f}}, {kOtherB, {90.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Blended), uint64_t{2}, "both are blended");
    check::equal(uploaded[1][0], 96.0f, "half way from N-1 to N");
}

void aLoneDrawsValuesAreItsOwn() {
    // One draw of the shader holds nothing in common with another, so what
    // moved is its own, and it passed nowhere near the middle.
    std::vector<Draw> latest{{kBlockA, {2.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f}}}, {{kBlockB, {9.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Unverified), uint64_t{1}, "it is unverified");
    check::equal(uploaded[0][0], 2.0f, "and drawn as the title drew it");
}

// Three tiles of the sea drawn in one pass and one in another, each handed
// its own place and its pass's view: {own, pass view, own}. Two blocks each,
// as the title double-buffers them.
constexpr uint32_t kTileA = 0xf4004000;
constexpr uint32_t kTileB = 0xf4084000;
constexpr uint32_t kFarA = 0xf4005000;
constexpr uint32_t kFarB = 0xf4085000;

void anUnverifiedObjectIsSeenThroughTheInBetweenCamera() {
    // The bobbing tile turned back between frames, so nothing in N-1 is
    // where it passed through; its pass's view moved as every tile's did.
    // Its last value is 7 at N as the first tile's is, but not at N-2: that
    // one is its own.
    std::vector<Draw> twoBack{{kBlockA, {0.0f, 100.0f, 6.0f}},
                              {kOtherA, {10.0f, 100.0f, 0.0f}},
                              {kTileA, {20.0f, 100.0f, 5.0f}},
                              {kFarA, {30.0f, 200.0f, 0.0f}}};
    std::vector<Draw> oneBack{{kBlockB, {1.0f, 101.0f, 6.5f}},
                              {kOtherB, {11.0f, 101.0f, 0.0f}},
                              {kTileB, {29.0f, 101.0f, 40.0f}},
                              {kFarB, {31.0f, 201.0f, 0.0f}}};
    std::vector<Draw> latest{{kBlockA, {2.0f, 102.0f, 7.0f}},
                             {kOtherA, {12.0f, 102.0f, 0.0f}},
                             {kTileA, {21.0f, 102.0f, 7.0f}},
                             {kFarA, {32.0f, 202.0f, 0.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, twoBack, oneBack, latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Unverified), uint64_t{1}, "the bobbing tile is unverified");
    check::equal(uploaded[2][1], 101.5f, "yet drawn with its pass's view half way");
    check::equal(uploaded[2][0], 21.0f, "at its own place in N");
    check::equal(uploaded[2][2], 7.0f, "and a value shared in N alone is its own");
    check::equal(uploaded[3][1], 201.5f, "the other pass's view is its own tile's");
    check::equal(blend.planner().unverifiedSharingValues(), uint64_t{1}, "counted");
    check::equal(blend.planner().valuesShared(), uint64_t{1}, "with the values taken");
}

void aPassValueIsOneValueInEveryShaderThatReadsIt() {
    // Two casters blended, drawing the pier into the light's map with the
    // light second; the pier, unverified, looks itself up in that map with
    // the light first, in a shader of its own. It must hold the light the
    // casters drew it with, or it shadows itself.
    std::vector<Draw> twoBack{{kBlockA, {0.0f, 100.0f}},
                              {kOtherA, {10.0f, 100.0f}},
                              {kTileA, {100.0f, 20.0f}, kPierShader}};
    std::vector<Draw> oneBack{{kBlockB, {1.0f, 101.0f}},
                              {kOtherB, {11.0f, 101.0f}},
                              {kTileB, {101.0f, 29.0f}, kPierShader}};
    std::vector<Draw> latest{{kBlockA, {2.0f, 102.0f}},
                             {kOtherA, {12.0f, 102.0f}},
                             {kTileA, {102.0f, 21.0f}, kPierShader}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, twoBack, oneBack, latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Unverified), uint64_t{1}, "the pier is unverified");
    check::equal(uploaded[2][0], 101.5f, "yet holds the light as the casters drew it");
    check::equal(uploaded[2][1], 21.0f, "and its own value at N");
}

// A 4x4 one row after another: the camera turned by `angle` about the
// vertical and stepped `step` along its view, times an object's own matrix
// `place`, as a model-view-projection the title hands each object.
std::vector<float> seenFrom(float angle, float step, const std::array<float, 16>& place) {
    std::array<float, 16> camera{
        std::cos(angle),  0.0f, std::sin(angle), 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -std::sin(angle), 0.0f, std::cos(angle), step, 0.0f, 0.0f, 0.5f, 1.0f};
    std::vector<float> product(16, 0.0f);
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            for (size_t k = 0; k < 4; ++k) {
                product[(row * 4) + column] += camera[(row * 4) + k] * place[(k * 4) + column];
            }
        }
    }
    return product;
}

std::array<float, 16> placedAt(float x, float y, float z) {
    return {1.0f, 0.0f, 0.0f, x, 0.0f, 1.0f, 0.0f, y, 0.0f, 0.0f, 1.0f, z, 0.0f, 0.0f, 0.0f, 1.0f};
}

std::vector<float> halfWay(const std::vector<float>& one, const std::vector<float>& other) {
    std::vector<float> between;
    for (size_t index = 0; index < one.size(); ++index) {
        between.push_back(one[index] + ((other[index] - one[index]) * kHalfway));
    }
    return between;
}

bool near(const std::vector<float>& one, const std::vector<float>& other) {
    for (size_t index = 0; index < one.size(); ++index) {
        if (std::abs(one[index] - other[index]) > 1e-4f * std::max(1.0f, std::abs(one[index]))) {
            return false;
        }
    }
    return one.size() == other.size();
}

void aMatrixEveryBlendedDrawChangedAlikeCarriesAnObjectDrawnAtN() {
    ShaderKey shader{kActorShader, 0, 0};
    SharedTransforms transforms;
    for (float x : {1.0f, 5.0f, -3.0f}) {
        std::array<float, 16> place = placedAt(x, 2.0f, 9.0f);
        transforms.addBlended(shader, seenFrom(0.2f, 1.0f, place),
                              halfWay(seenFrom(0.1f, 0.0f, place), seenFrom(0.2f, 1.0f, place)));
    }
    transforms.index();
    check::isTrue(transforms.windowsOf(shader) == std::vector<uint32_t>{0},
                  "the matrix every blended draw changed alike is found");
    std::array<float, 16> tree = placedAt(-7.0f, 0.0f, 20.0f);
    std::vector<float> latest = seenFrom(0.2f, 1.0f, tree);
    std::vector<float> out = latest;
    std::vector<uint8_t> keep(latest.size(), 0);
    check::equal(transforms.carry(shader, latest, keep, out), size_t{1}, "one window carried");
    check::isTrue(near(out, halfWay(seenFrom(0.1f, 0.0f, tree), latest)),
                  "to where it would have been blended, had it been");
}

void aMatrixTheBlendedDrawsChangedEachTheirOwnWayIsNoCamera() {
    ShaderKey shader{kActorShader, 0, 0};
    SharedTransforms transforms;
    float turn = 0.1f;
    for (float x : {1.0f, 5.0f, -3.0f}) {
        std::array<float, 16> place = placedAt(x, 2.0f, 9.0f);
        transforms.addBlended(
            shader, seenFrom(turn * 2.0f, 1.0f, place),
            halfWay(seenFrom(turn, 0.0f, place), seenFrom(turn * 2.0f, 1.0f, place)));
        turn += 0.1f;
    }
    transforms.index();
    check::isTrue(transforms.windowsOf(shader).empty(), "no window is shared");
    std::vector<float> latest = seenFrom(0.2f, 1.0f, placedAt(-7.0f, 0.0f, 20.0f));
    std::vector<float> out = latest;
    std::vector<uint8_t> keep(latest.size(), 0);
    check::equal(transforms.carry(shader, latest, keep, out), size_t{0}, "nothing is carried");
    check::isTrue(out == latest, "and the object is drawn as the title drew it");
}

void aWindowSharedValuesAlreadyDrewIsLeftToThem() {
    ShaderKey shader{kActorShader, 0, 0};
    SharedTransforms transforms;
    for (float x : {1.0f, 5.0f, -3.0f}) {
        std::array<float, 16> place = placedAt(x, 2.0f, 9.0f);
        transforms.addBlended(shader, seenFrom(0.2f, 1.0f, place),
                              halfWay(seenFrom(0.1f, 0.0f, place), seenFrom(0.2f, 1.0f, place)));
    }
    transforms.index();
    std::vector<float> latest = seenFrom(0.2f, 1.0f, placedAt(-7.0f, 0.0f, 20.0f));
    std::vector<float> out = latest;
    std::vector<uint8_t> keep(latest.size(), 1);
    check::equal(transforms.carry(shader, latest, keep, out), size_t{0}, "nothing carried");
}

void anUnverifiedObjectsOwnViewIsTurnedWithTheCamera() {
    // Four objects of one shader, each handed its matrix with the camera in
    // it, the camera turning. The tree's draw in N-1 is not where it passed
    // through -- its blocks name another draw -- so it is unverified, and
    // held at N it would stand where N's camera put it, amid a world drawn
    // from between the two.
    constexpr uint32_t kRockA = 0xf4003000;
    constexpr uint32_t kRockB = 0xf4083000;
    constexpr uint32_t kTreeA = 0xf4004000;
    constexpr uint32_t kTreeB = 0xf4084000;
    std::array<float, 16> tree = placedAt(-7.0f, 0.0f, 20.0f);
    auto frame = [&](float angle, float step, uint32_t house, uint32_t other, uint32_t rock,
                     uint32_t treeBlock, const std::vector<float>& treeValues) {
        return std::vector<Draw>{{house, seenFrom(angle, step, placedAt(1.0f, 2.0f, 9.0f))},
                                 {other, seenFrom(angle, step, placedAt(5.0f, 2.0f, 9.0f))},
                                 {rock, seenFrom(angle, step, placedAt(-3.0f, 2.0f, 9.0f))},
                                 {treeBlock, treeValues}};
    };
    std::vector<float> treeAway = seenFrom(0.1f, 0.5f, placedAt(40.0f, 0.0f, -20.0f));
    std::vector<Draw> latest =
        frame(0.2f, 1.0f, kBlockA, kOtherA, kRockA, kTreeA, seenFrom(0.2f, 1.0f, tree));
    ObjectBlend blend{kHalfway};
    armAfter(blend, frame(0.0f, 0.0f, kBlockA, kOtherA, kRockA, kTreeA, seenFrom(0.0f, 0.0f, tree)),
             frame(0.1f, 0.5f, kBlockB, kOtherB, kRockB, kTreeB, treeAway), latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Blended), uint64_t{3}, "the three others are blended");
    check::equal(blend.objects(Outcome::Unverified), uint64_t{1}, "the tree is unverified");
    check::isTrue(near(uploaded[3], halfWay(seenFrom(0.1f, 0.5f, tree), latest[3].values)),
                  "yet drawn as it would have been blended, turned with the camera");
    check::equal(blend.planner().unblendedCarried(), uint64_t{1}, "counted");
    check::equal(blend.planner().transformsCarried(), uint64_t{1}, "with its one matrix");
    check::equal(blend.planner().leftAtN(), uint64_t{0}, "so none is left at N");
}

void anObjectNewAtNIsSeenThroughTheViewEveryDrawShares() {
    // Two objects stand still in a turning view; a third is drawn for the
    // first time at N, as a palm is when it comes into view or its near model
    // takes over. With no N-2 it cannot be matched, and drawn at N it would
    // stand where N's camera put it: it takes the view the others drew.
    std::vector<Draw> twoBack{{kBlockA, {0.0f, 5.0f}}, {kOtherA, {0.0f, 6.0f}}};
    std::vector<Draw> oneBack{{kBlockB, {1.0f, 5.0f}}, {kOtherB, {1.0f, 6.0f}}};
    std::vector<Draw> latest{
        {kBlockA, {2.0f, 5.0f}}, {kOtherA, {2.0f, 6.0f}}, {kTileA, {2.0f, 9.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, twoBack, oneBack, latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Unmatched), uint64_t{1}, "the new object is unmatched");
    check::equal(uploaded[2][0], 1.5f, "yet drawn with the view the others drew");
    check::equal(uploaded[2][1], 9.0f, "and its own value as the title drew it");
    check::equal(blend.planner().leftAtN(), uint64_t{0}, "so none is left at N");
}

void aValueADrawHoldsUnmovedIsNotTakenByANewObject() {
    // One object moved to 7 at N; another held 7 all along. A new object's 7
    // is either's, and the one held unmoved says it need not move.
    std::vector<Draw> twoBack{{kBlockA, {5.0f, 1.0f}}, {kOtherA, {7.0f, 2.0f}}};
    std::vector<Draw> oneBack{{kBlockB, {6.0f, 1.0f}}, {kOtherB, {7.0f, 2.0f}}};
    std::vector<Draw> latest{
        {kBlockA, {7.0f, 1.0f}}, {kOtherA, {7.0f, 2.0f}}, {kTileA, {7.0f, 9.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, twoBack, oneBack, latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Unmatched), uint64_t{1}, "the new object is unmatched");
    check::equal(uploaded[0][0], 6.5f, "the mover is drawn half way");
    check::equal(uploaded[2][0], 7.0f, "the new object as the title drew it");
    check::equal(blend.planner().leftAtN(), uint64_t{1}, "left at N");
}

void aValueBlendedDrawsDisagreeOnIsNotShared() {
    // Two blended tiles held the unverified tile's view at N-2 and N but
    // passed through different values in N-1: which of them it shares is
    // not known, so it is drawn as the title drew it.
    std::vector<Draw> twoBack{
        {kBlockA, {0.0f, 100.0f}}, {kOtherA, {10.0f, 100.0f}}, {kTileA, {20.0f, 100.0f}}};
    std::vector<Draw> oneBack{
        {kBlockB, {1.0f, 101.0f}}, {kOtherB, {11.0f, 100.8f}}, {kTileB, {29.0f, 101.0f}}};
    std::vector<Draw> latest{
        {kBlockA, {2.0f, 102.0f}}, {kOtherA, {12.0f, 102.0f}}, {kTileA, {21.0f, 102.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, twoBack, oneBack, latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Blended), uint64_t{2}, "both neighbours are blended");
    check::equal(blend.objects(Outcome::Unverified), uint64_t{1}, "the tile is unverified");
    check::isTrue(uploaded[2] == latest[2].values, "and drawn as the title drew it");
    check::equal(blend.planner().unverifiedSharingValues(), uint64_t{0}, "sharing nothing");
    check::equal(blend.planner().leftAtN(), uint64_t{1}, "counted as left at N");
}

void theLightsMapAndItsLookUpAreDrawnAtN() {
    // A caster drawn into the shadow map -- depth alone -- and the pixel
    // stage looking the map up both moved with the light, as the walker
    // moved of itself: only the walker is drawn between.
    auto frame = [](float walker, float light) {
        return std::vector<Draw>{
            {kBlockA, {walker, 7.0f}},
            {kOtherA, {5.0f, light}, kActorShader, 0, 0, false},
            {kTileA, {light, light}, kPierShader, 0, ObjectPlanner::kPixelStage}};
    };
    std::vector<Draw> latest = frame(2.0f, 12.0f);
    ObjectBlend blend{kHalfway};
    armAfter(blend, frame(0.0f, 10.0f), frame(1.0f, 11.0f), latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Shading), uint64_t{2}, "the caster and look-up shade");
    check::equal(uploaded[0][0], 1.5f, "the walker is drawn half way");
    check::isTrue(uploaded[1] == latest[1].values, "the caster as the title drew it");
    check::isTrue(uploaded[2] == latest[2].values, "and the look-up as the title drew it");
}

void anObjectFarFromTheOriginIsKnownThroughItsRounding() {
    // A boat rocking far from the origin: its turn passes half way, but its
    // place moves one unit in the float's last place, which the title rounds
    // at N-1 onto where it stood. Rounding is not a step off its path.
    float far = 196608.0f;
    float farther = std::nextafter(far, 1.0e6f);
    std::vector<Draw> latest{{kBlockA, {0.698f, farther}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.692f, far}}}, {{kBlockB, {0.695f, far}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Blended), uint64_t{1}, "the boat is blended");
    check::equal(uploaded[0][0], 0.5f * (0.695f + 0.698f), "and turned half way");
}

void anObjectKnownByItsBlocksThatStoppedAtNMinusOneIsHeld() {
    // Walked long enough for its blocks to be paired, then stopped a frame
    // before N: its own draw at N-1 holds N, which is where it stands
    // between.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    record(blend, {{kBlockA, {0.0f, 7.0f}}});
    record(blend, {{kBlockB, {1.0f, 7.0f}}});
    record(blend, {{kBlockA, {2.0f, 7.0f}}});
    record(blend, {{kBlockB, {3.0f, 7.0f}}});
    std::vector<Draw> stopped{{kBlockA, {3.0f, 7.0f}}};
    record(blend, stopped);
    blend.armOnce();
    auto uploaded = replay(blend, stopped);
    check::equal(blend.objects(Outcome::Held), uint64_t{1}, "it is known by its blocks");
    check::equal(uploaded[0][0], 3.0f, "and drawn where it stopped");
}

void anObjectKnownByItsBlocksThatStoodUntilNMinusOneIsBlended() {
    // Walked, stood, and moved again after N-1: it sets off from where it
    // stood, and is its own draw there.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    record(blend, {{kBlockA, {0.0f, 7.0f}}});
    record(blend, {{kBlockB, {1.0f, 7.0f}}});
    record(blend, {{kBlockA, {2.0f, 7.0f}}});
    record(blend, {{kBlockB, {2.0f, 7.0f}}});
    std::vector<Draw> moved{{kBlockA, {3.0f, 7.0f}}};
    record(blend, moved);
    blend.armOnce();
    auto uploaded = replay(blend, moved);
    check::equal(blend.objects(Outcome::Blended), uint64_t{1}, "it is blended");
    check::equal(uploaded[0][0], 2.5f, "half way from where it stood");
}

void anObjectFoundByItsValuesThatStoodUntilNMinusOneIsBlended() {
    // Stepped every other frame, it stood still at the start of each move
    // and its blocks were never paired: its search, run again, finds it
    // where it stood, half way from N-3 to N.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    uint64_t searchedAgain = 2 + wiiuport::interp::ObjectPlanner::kSearchRetryInterval;
    for (uint64_t frame = 0; frame < searchedAgain; ++frame) {
        record(blend,
               {{frame % 2 == 0 ? kBlockA : kBlockB, {static_cast<float>(frame / 2), 7.0f}}});
    }
    float stood = static_cast<float>(searchedAgain / 2 - 1);
    std::vector<Draw> stepped{{kBlockA, {stood + 1.0f, 7.0f}}};
    record(blend, stepped);
    blend.armOnce();
    auto uploaded = replay(blend, stepped);
    check::equal(blend.objects(Outcome::Blended), uint64_t{1}, "it is blended");
    check::equal(uploaded[0][0], stood + 0.5f, "half way from where it stood");
}

void aTileWhoseBlocksPassedToAnotherIsNotBlendedFromWhereTheOtherStood() {
    // Two sea tiles standing still under a value every tile moves in, their
    // blocks learned. At N the grid shifts and each tile's blocks pass to
    // the other: the draw they name stood still until N-1, a hundred units
    // from where the tile is.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    record(blend, {{kBlockA, {0.0f, 0.5f}}, {kOtherA, {100.0f, 0.5f}}});
    record(blend, {{kBlockB, {0.0f, 0.75f}}, {kOtherB, {100.0f, 0.75f}}});
    record(blend, {{kBlockA, {0.0f, 1.0f}}, {kOtherA, {100.0f, 1.0f}}});
    record(blend, {{kBlockB, {0.0f, 1.5f}}, {kOtherB, {100.0f, 1.5f}}});
    std::vector<Draw> shifted{{kBlockA, {100.0f, 2.0f}}, {kOtherA, {0.0f, 2.0f}}};
    record(blend, shifted);
    blend.armOnce();
    auto uploaded = replay(blend, shifted);
    check::isTrue(uploaded[0][0] == 100.0f && uploaded[1][0] == 0.0f,
                  "neither tile is drawn half way to the other");
}

void aDrawWithNoNumberWhereTheObjectHeldDoesNotStandWhereItStood() {
    // Found by its values, the draw in N-1 stands where the object stood in
    // what it moved, but holds no number where it held one: it does not
    // stand as the object stood, bit for bit.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    record(blend, {{kBlockA, {0.0f, 7.0f}}});
    record(blend, {{kBlockB, {9.0f, 7.0f}}});
    record(blend, {{kBlockA, {0.0f, 7.0f}}});
    record(blend, {{kBlockB, {0.0f, std::nanf("")}}});
    std::vector<Draw> moved{{kBlockA, {2.0f, 7.0f}}};
    record(blend, moved);
    blend.armOnce();
    replay(blend, moved);
    check::equal(blend.objects(Outcome::Unverified), uint64_t{1}, "it has no partner");
}

void aSwayTurningBackThroughTheValueItPassedIsBlended() {
    // Its angle in whole steps: up to the end of its swing, standing there
    // from N-2 to N-1, and back down through the very value it passed at
    // N-3 -- on its way from N-4, where a flip would have stood.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    record(blend, {{kBlockA, {0.0f, 7.0f}}});
    record(blend, {{kBlockB, {1.0f, 7.0f}}});
    record(blend, {{kBlockA, {2.0f, 7.0f}}});
    record(blend, {{kBlockB, {3.0f, 7.0f}}});
    record(blend, {{kBlockA, {3.0f, 7.0f}}});
    std::vector<Draw> back{{kBlockB, {2.0f, 7.0f}}};
    record(blend, back);
    blend.armOnce();
    auto uploaded = replay(blend, back);
    check::equal(blend.objects(Outcome::Blended), uint64_t{1}, "it is blended");
    check::equal(uploaded[0][0], 2.5f, "half way from where it turned");
}

void anObjectSeenOnlyStandingIsDrawnAtNSettingOff() {
    // Standing from N-4 to N-1: nothing tells it from another object the
    // title's blocks passed to it.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    record(blend, {{kBlockA, {0.0f, 7.0f}}});
    record(blend, {{kBlockB, {1.0f, 7.0f}}});
    record(blend, {{kBlockA, {2.0f, 7.0f}}});
    record(blend, {{kBlockB, {2.0f, 7.0f}}});
    record(blend, {{kBlockA, {2.0f, 7.0f}}});
    record(blend, {{kBlockB, {2.0f, 7.0f}}});
    std::vector<Draw> setOff{{kBlockA, {3.0f, 7.0f}}};
    record(blend, setOff);
    blend.armOnce();
    auto uploaded = replay(blend, setOff);
    check::equal(blend.objects(Outcome::Blended), uint64_t{0}, "it is not blended");
    check::equal(uploaded[0][0], 3.0f, "drawn at N");
}

void aStillSpriteWhoseBlocksPassToAnotherIsNotBlendedToIt() {
    // Seen on the shore: a sprite's blocks name a large one standing still
    // from N-3 to N-1 and a small one at N-4 and N. Neither frame on either
    // side of the stand is half way to the other.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    record(blend, {{kBlockA, {0.0f, 7.0f}}});
    record(blend, {{kBlockB, {12.0f, 7.0f}}});
    record(blend, {{kBlockA, {24.0f, 7.0f}}});
    record(blend, {{kBlockB, {72.0f, 7.0f}}});
    record(blend, {{kBlockA, {72.0f, 7.0f}}});
    record(blend, {{kBlockB, {72.0f, 7.0f}}});
    std::vector<Draw> small{{kBlockA, {24.0f, 7.0f}}};
    record(blend, small);
    blend.armOnce();
    auto uploaded = replay(blend, small);
    check::equal(blend.objects(Outcome::Blended), uint64_t{0}, "it is not blended");
    check::equal(uploaded[0][0], 24.0f, "the small one is drawn at N");
}

void anObjectTheTitleMovesEveryOtherFrameIsBlended() {
    // Swaying in the wind a step every other frame: it stood from N-2 to
    // N-1, and N-1 is midway from N-3 to N.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    record(blend, {{kBlockA, {-1.0f, 7.0f}}});
    record(blend, {{kBlockB, {0.0f, 7.0f}}});
    record(blend, {{kBlockA, {1.0f, 7.0f}}});
    record(blend, {{kBlockB, {1.0f, 7.0f}}});
    std::vector<Draw> stepped{{kBlockA, {2.0f, 7.0f}}};
    record(blend, stepped);
    blend.armOnce();
    auto uploaded = replay(blend, stepped);
    check::equal(blend.objects(Outcome::Blended), uint64_t{1}, "it is blended");
    check::equal(uploaded[0][0], 1.5f, "half way from N-1");
}

void anObjectKnownByItsBlocksThatTurnedBackIsBlended() {
    // Swaying in the wind: past its mark at N-1 and back by N. Its own draw
    // is where it passed, however far from the midpoint.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    record(blend, {{kBlockA, {0.0f, 7.0f}}});
    record(blend, {{kBlockB, {1.0f, 7.0f}}});
    record(blend, {{kBlockA, {2.0f, 7.0f}}});
    record(blend, {{kBlockB, {4.0f, 7.0f}}});
    std::vector<Draw> back{{kBlockA, {3.0f, 7.0f}}};
    record(blend, back);
    blend.armOnce();
    auto uploaded = replay(blend, back);
    check::equal(blend.objects(Outcome::Blended), uint64_t{1}, "it is known by its blocks");
    check::equal(uploaded[0][0], 3.5f, "and drawn half way back");
}

void blocksReusedByAnotherObjectDoNotNameThePartner() {
    // At N-1 the two walkers' blocks were handed over to each other: the
    // draw each one's blocks name is the other walker's.
    ObjectBlend blend{kHalfway};
    blend.setPlanning(true);
    record(blend, {{kBlockA, {0.0f, 7.0f}}, {kOtherA, {100.0f, 7.0f}}});
    record(blend, {{kBlockB, {1.0f, 7.0f}}, {kOtherB, {101.0f, 7.0f}}});
    record(blend, {{kBlockA, {2.0f, 7.0f}}, {kOtherA, {102.0f, 7.0f}}});
    record(blend, {{kBlockB, {103.0f, 7.0f}}, {kOtherB, {3.0f, 7.0f}}});
    std::vector<Draw> latest{{kBlockA, {4.0f, 7.0f}}, {kOtherA, {104.0f, 7.0f}}};
    record(blend, latest);
    blend.armOnce();
    auto uploaded = replay(blend, latest);
    check::equal(uploaded[0][0], 3.5f, "each is drawn half way from its own draw");
    check::equal(uploaded[1][0], 103.5f, "not from the one its blocks named");
}

void aMoveTooSmallToHalveIsNotDrawnBetween() {
    // One ulp from N-1 to N: half way rounds back onto N-1, which is not a
    // frame between the two.
    float one = 1.0f;
    float up = std::nextafter(one, 2.0f);
    float down = one - (up - one);
    std::vector<Draw> latest{{kBlockA, {up, 7.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {down, 7.0f}}}, {{kBlockB, {one, 7.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(blend.objects(Outcome::Outside), uint64_t{1}, "it is counted as outside");
    check::equal(uploaded[0][0], up, "and drawn as the title drew it");
}

void aPartnerWhoseMovingValuesAreNotNumbersIsNoPartner() {
    std::vector<Draw> latest{{kBlockA, {2.0f, 7.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, 7.0f}}}, {{kBlockB, {std::nanf(""), 7.0f}}}, latest);
    replay(blend, latest);
    check::equal(blend.objects(Outcome::Unverified), uint64_t{1},
                 "nothing it moved in can be checked, so it is unverified");
}

void anObjectThatStoppedAtNMinusOneIsDrawnWhereItStopped() {
    // Moved from N-2 to N-1 and stood still since: between N-1 and N it is at
    // N, not anywhere on the way from N-2.
    std::vector<Draw> latest{{kBlockA, {2.0f, 7.0f}}};
    ObjectBlend blend{kHalfway};
    armAfter(blend, {{kBlockA, {0.0f, 7.0f}}}, {{kBlockB, {2.0f, 7.0f}}}, latest);
    auto uploaded = replay(blend, latest);
    check::equal(uploaded[0][0], 2.0f, "it is drawn where it stopped");
}

constexpr uint64_t kCameraShader = 0xcafe;
constexpr uint32_t kCameraBlock = 0xf4003000;

// A camera turned and standing far out at sea, where rounding a pose through
// a quaternion and back shows in the low bits for most angles, not all.
std::vector<float> cameraView(float angle) {
    float c = std::cos(angle);
    float s = std::sin(angle);
    return {c, 0.0f, -s, 301234.5f + angle, 0.0f, 1.0f, 0.0f, -42.25f, s, 0.0f, c, -287654.25f};
}

// How many of a held-still world's replayed draws come out byte-identical to
// the title's, with the camera held at `angle`.
size_t identicalDrawsWithTheCameraAt(float angle) {
    float integer = 4.2e-45f;
    std::vector<Draw> still{{kCameraBlock, cameraView(angle), kCameraShader},
                            {kBlockA, {2.0f, 7.0f, 3.0f}},
                            {kOtherA, {5.0f, integer, 5.0f}},
                            {kBlockA, {-1.0f, 0.1f, 1e-3f}, kOutline}};
    ObjectBlend objects{kHalfway};
    armAfter(objects, still, still, still);
    auto view = wiiuport::interp::Transform3x4::fromRowMajor(cameraView(angle).data());
    wiiuport::interp::TransformSubstitution camera;
    camera.armOnce({{{kCameraShader, 0, 0}, 0, view, view}}, kHalfway);
    wiiuport::interp::ReplayBlend filter{objects, camera};
    size_t identical = 0;
    for (const Draw& draw : still) {
        ReplayedDraw replayed(draw);
        filter.onRuntimeAssembly(replayed.assembly);
        identical += std::memcmp(replayed.values.data(), draw.values.data(),
                                 draw.values.size() * sizeof(float)) == 0
                         ? 1
                         : 0;
    }
    check::equal(objects.objects(Outcome::Held), uint64_t{4}, "every object is held");
    check::equal(camera.assembliesSubstituted(), uint64_t{1},
                 "and the camera is written: the comparison sees its bits");
    return identical;
}

constexpr size_t kDrawsPerWorld = 4;

void aHeldStillWorldReplaysByteIdenticalToTheTitlesFrame() {
    for (int step = 0; step < 16; ++step) {
        check::equal(identicalDrawsWithTheCameraAt(static_cast<float>(step) * 0.1f), kDrawsPerWorld,
                     "every replayed draw is the title's, byte for byte");
    }
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

// Frame N as the census sees it: the walker blended, the stander held, and
// two outline draws that are new.
const std::vector<Draw> kCensusLatest{{kBlockA, {2.0f, 7.0f}},
                                      {kOtherA, {5.0f, 5.0f}},
                                      {0xf4005000, {1.0f, 2.0f, 3.0f}, kOutline},
                                      {0xf4006000, {4.0f}, kOutline}};

void aCensusGroupsTheFramesObjectsByShaderMostUnblendedFirst() {
    ObjectBlend blend{kHalfway};
    blend.requestCensus(1);
    armAfter(blend, kWalkTwoBack, kWalkOneBack, kCensusLatest);
    auto census = blend.census();
    check::isTrue(census.has_value(), "a requested census is taken once a frame is planned");
    check::equal(census->frames, uint64_t{1}, "of the one frame asked for");
    check::equal(census->objects, uint64_t{4}, "of every object in the frame");
    check::equal(census->shaders, uint64_t{2}, "under the shaders that drew them");
    check::equal(census->rows[0].shader.baseHash, kOutline, "the un-blended shader first");
    check::equal(census->rows[0].unblended(), uint64_t{2}, "with both its draws unmatched");
    check::equal(census->rows[0].mostValues, uint64_t{3}, "and its widest draw's values");
    const auto& actor = census->rows[1].outcomes;
    check::equal(actor[static_cast<size_t>(Outcome::Blended)], uint64_t{1}, "the walker");
    check::equal(actor[static_cast<size_t>(Outcome::Held)], uint64_t{1}, "and the stander");
    check::equal(census->outcomes[static_cast<size_t>(Outcome::Unmatched)], uint64_t{2},
                 "and the totals agree with the rows");
}

void aCensusAddsUpTheFramesAskedForAndNoMore() {
    ObjectBlend blend{kHalfway};
    armAfter(blend, kWalkTwoBack, kWalkOneBack, kWalkLatest);
    check::isTrue(!blend.census().has_value(), "no census nobody asked for");
    blend.requestCensus(2);
    record(blend, kWalkTwoBack);
    check::isTrue(!blend.census().has_value(), "none after one of the two frames");
    record(blend, kWalkOneBack);
    auto census = blend.census();
    check::isTrue(census.has_value(), "one after both");
    check::equal(census->frames, uint64_t{2}, "counting both frames");
    check::equal(census->objects, uint64_t{4}, "and both frames' objects");
    record(blend, kWalkLatest);
    check::equal(blend.census()->frames, uint64_t{2}, "and nothing after");
}

void aCensusOfAnUnplannedFrameIsRefused() {
    wiiuport::interp::ObjectPlanner planner{kHalfway};
    wiiuport::interp::CensusTally tally;
    bool refused = false;
    try {
        tally.add(planner);
    } catch (const std::logic_error&) {
        refused = true;
    }
    check::isTrue(refused, "a frame never planned would read as all unmatched");
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
    aValueTheObjectHeldIsDrawnAsItsOwn();
    aTileWhoseBlocksPassedToAnotherIsFoundByItsValues();
    anObjectDrawnFromBlocksNewToItIsFoundByItsValues();
    anObjectFoundStandingStillByItsValuesIsHeld();
    aFailedSearchByValuesDoesNotDelayTheSearchByBlocks();
    aFailedSearchWaitsBeforeItIsRunAgain();
    aFrameWithNoDrawsStillLeavesTheOneBeforeSearchable();
    aKeptSnapshotIsPlannedAgainAsTheProductPlansIt();
    aValueThatIsNotANumberIsTakenFromTheLaterFrame();
    twoDrawsFromOneBlockAreTwoObjects();
    aReplayOutOfStepWithTheRecordingStopsWriting();
    nothingIsWrittenWhenNotArmed();
    aPartnerIsFoundAmongManyDrawsOfItsShader();
    aPartnerWithNoNumberWhereItsShaderIsOrderedIsStillFound();
    aFrameOfManyDrawsIsPlannedWhileItIsDrawn();
    aBlendIsTakenFromItsPartnerNotAssumedSteady();
    aBlockAllocatedAfreshEveryFrameDoesNotHideItsObject();
    aValueFlippingEveryFrameIsNotAveraged();
    aMoveTooSmallToHalveIsNotDrawnBetween();
    anObjectFarFromTheOriginIsKnownThroughItsRounding();
    anObjectKnownByItsBlocksThatStoppedAtNMinusOneIsHeld();
    anObjectKnownByItsBlocksThatStoodUntilNMinusOneIsBlended();
    anObjectFoundByItsValuesThatStoodUntilNMinusOneIsBlended();
    aTileWhoseBlocksPassedToAnotherIsNotBlendedFromWhereTheOtherStood();
    aDrawWithNoNumberWhereTheObjectHeldDoesNotStandWhereItStood();
    aSwayTurningBackThroughTheValueItPassedIsBlended();
    anObjectSeenOnlyStandingIsDrawnAtNSettingOff();
    aStillSpriteWhoseBlocksPassToAnotherIsNotBlendedToIt();
    anObjectTheTitleMovesEveryOtherFrameIsBlended();
    anObjectKnownByItsBlocksThatTurnedBackIsBlended();
    blocksReusedByAnotherObjectDoNotNameThePartner();
    aFlippingValueFarLargerThanTheMoveDoesNotHideThePartner();
    aValueEveryDrawHoldsDoesNotDecideThePartner();
    drawsTheFrameAloneMovesAreBlendedWithAnyCandidate();
    aLoneDrawsValuesAreItsOwn();
    anUnverifiedObjectIsSeenThroughTheInBetweenCamera();
    aValueBlendedDrawsDisagreeOnIsNotShared();
    aPassValueIsOneValueInEveryShaderThatReadsIt();
    theLightsMapAndItsLookUpAreDrawnAtN();
    aMatrixEveryBlendedDrawChangedAlikeCarriesAnObjectDrawnAtN();
    aMatrixTheBlendedDrawsChangedEachTheirOwnWayIsNoCamera();
    aWindowSharedValuesAlreadyDrewIsLeftToThem();
    anUnverifiedObjectsOwnViewIsTurnedWithTheCamera();
    anObjectNewAtNIsSeenThroughTheViewEveryDrawShares();
    aValueADrawHoldsUnmovedIsNotTakenByANewObject();
    aPartnerWhoseMovingValuesAreNotNumbersIsNoPartner();
    anObjectThatStoppedAtNMinusOneIsDrawnWhereItStopped();
    aHeldStillWorldReplaysByteIdenticalToTheTitlesFrame();
    turningPlanningOffForgetsTheFramesItHeld();
    aCensusGroupsTheFramesObjectsByShaderMostUnblendedFirst();
    aCensusAddsUpTheFramesAskedForAndNoMore();
    aCensusOfAnUnplannedFrameIsRefused();
}

} // namespace wiiuport::tests
