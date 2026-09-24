#include "check.h"
#include "object_blend_fixture.h"
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

using wiiuport::interp::ObjectBlend;
using Outcome = wiiuport::interp::ObjectBlend::Outcome;
using wiiuport::interp::ObjectPlanner;
using wiiuport::interp::ShaderKey;
using wiiuport::interp::SharedTransforms;
using wiiuport::tests::object_blend::armAfter;
using wiiuport::tests::object_blend::Draw;
using wiiuport::tests::object_blend::kActorShader;
using wiiuport::tests::object_blend::kBlockA;
using wiiuport::tests::object_blend::kBlockB;
using wiiuport::tests::object_blend::kHalfway;
using wiiuport::tests::object_blend::kOtherA;
using wiiuport::tests::object_blend::kOtherB;
using wiiuport::tests::object_blend::kPierShader;
using wiiuport::tests::object_blend::replay;

namespace {

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
    between.reserve(one.size());
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

// A rock and a tree drawn by one shader, each from its own pair of blocks.
constexpr uint32_t kRockA = 0xf4003000;
constexpr uint32_t kRockB = 0xf4083000;
constexpr uint32_t kTreeA = 0xf4004000;
constexpr uint32_t kTreeB = 0xf4084000;

void anUnverifiedObjectsOwnViewIsTurnedWithTheCamera() {
    // Four objects of one shader, each handed its matrix with the camera in
    // it, the camera turning. The tree's draw in N-1 is not where it passed
    // through -- its blocks name another draw -- so it is unverified, and
    // held at N it would stand where N's camera put it, amid a world drawn
    // from between the two.
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

} // namespace

namespace wiiuport::tests {

// Values a draw shares with the rest of the frame: the camera, a pass, the
// lights.
void runObjectBlendSharedValueTests() {
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
}

} // namespace wiiuport::tests
