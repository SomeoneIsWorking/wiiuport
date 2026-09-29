#include "check.h"
#include "suites.h"
#include "wiiuport/title/PoseBlend.h"
#include "wiiuport/title/PoseByShader.h"

#include <array>
#include <string>
#include <vector>

using wiiuport::title::PoseBlend;
using wiiuport::title::PoseByShader;

namespace {

constexpr uint64_t kShader = 0x1557c18f92f3bcb9;
constexpr uint32_t kNodeA = 0x027ff88c;
constexpr uint32_t kNodeB = 0x027ff9c0;

// The two offsets the real title measured: the per-object pose at 12, and the pass's value at 76
// which no other object shares, so the table refuses it.
constexpr uint32_t kObjectOffset = 12;
constexpr uint32_t kPassOffset = 76;

PoseByShader::Entry perObjectAt(uint32_t byteOffset) {
    PoseByShader::Entry entry;
    entry.byteOffset = byteOffset;
    entry.moved = 21730;
    entry.compared = 21879;
    entry.otherObjects = 15;
    entry.otherObjectsSame = 0;
    return entry;
}

// **One float, not two.** The second was never used by the fixture, so the pair was swappable by
// mistake and the mistake compiled -- a `y` of zero and a `y` of the other value are both plausible
// at a call and neither is a type error.
void putPose(std::vector<float>& words, size_t at, float x) {
    // Three rows of three and a translation, which is how `TransformShape` reads a 3x4. The
    // translation is what moves between ticks, so it is the only part that needs to differ.
    const float pose[PoseBlend::kWords] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                           0.0f, 0.0f, 1.0f, x,    0.0f, 0.0f};
    for (size_t word = 0; word < PoseBlend::kWords; word++) {
        words[at + word] = pose[word];
    }
}

// **The in-between frame is the midpoint of tick N-1 and tick N, and it is the first of the two
// paints.** Two ticks, two objects, and the arithmetic is the test's own: node A at x=0 then x=10
// writes 5 on the in-between paint; node B does the same at its own value, and neither object sees
// the other's pose, because the identity is the node.
void theInBetweenFrameIsTheMidpointOfTheLastTwoTicksAndOnlyOnTheInBetweenPaint() {
    PoseByShader poses;
    std::string refusal;
    poses.offer(kShader, 0, perObjectAt(kObjectOffset), refusal);
    PoseBlend blend(poses);

    // **Tick N-1**: the title's own paint, pose at x=0. Held, nothing written.
    {
        std::vector<float> words(64, 0.0f);
        putPose(words, kObjectOffset / 4, 0.0f);
        blend.onAssemblyAtOffset(words.data(), words.size(), kObjectOffset, kShader, kNodeA, false);
    }
    // **Tick N**: the in-between paint, pose at x=10, written as 5.
    {
        std::vector<float> words(64, 0.0f);
        putPose(words, kObjectOffset / 4, 10.0f);
        blend.onAssemblyAtOffset(words.data(), words.size(), kObjectOffset, kShader, kNodeA, true);
        check::equal(words[kObjectOffset / 4 + 9], 5.0f,
                     "the translation is the midpoint of 0 and 10, which is the lerp of N-1 to N");
        check::equal(words[kObjectOffset / 4 + 0], 1.0f,
                     "and the rotation is unchanged, because it "
                     "was the same at both ticks");
    }
    // **And the tick's own paint, which must be the title's own value and not the midpoint.**
    {
        std::vector<float> words(64, 0.0f);
        putPose(words, kObjectOffset / 4, 10.0f);
        blend.onAssemblyAtOffset(words.data(), words.size(), kObjectOffset, kShader, kNodeA, false);
        check::equal(words[kObjectOffset / 4 + 9], 10.0f,
                     "the tick's own paint carries the tick's own value, so the midpoint is not "
                     "presented in place of the frame");
    }
    // A second object, at its own pose, is unaffected by the first's: the identity is the node.
    {
        std::vector<float> words(64, 0.0f);
        putPose(words, kObjectOffset / 4, 0.0f);
        blend.onAssemblyAtOffset(words.data(), words.size(), kObjectOffset, kShader, kNodeB, false);
        putPose(words, kObjectOffset / 4, 4.0f);
        blend.onAssemblyAtOffset(words.data(), words.size(), kObjectOffset, kShader, kNodeB, true);
        check::equal(words[kObjectOffset / 4 + 9], 2.0f,
                     "the second node is the midpoint of its own two ticks and not the first's");
    }

    const auto tally = blend.tally();
    check::equal(tally.lerped, uint64_t{2}, "two draws were blended");
    check::equal(tally.firstSight, uint64_t{2}, "and two were the first time a pose was seen");
    check::equal(tally.notInBetween, uint64_t{1}, "while one was held on the tick's own paints");
    check::equal(tally.wordsWritten, uint64_t{2 * PoseBlend::kWords}, "twelve words each");
    check::equal(tally.inBetweenKnown, uint64_t{2},
                 "and both in-between draws were over a pair that was already held, which is the "
                 "denominator the hit rate is taken of");
    check::equal(tally.refreshed, uint64_t{5}, "while five assemblies refreshed a held pose");
    check::equal(blend.held(), uint64_t{2},
                 "and two pairs are held, one per node -- two, not five, because a pair is a node "
                 "and not an assembly");
    check::equal(blend.heldOnFallbackIdentity(), uint64_t{0},
                 "neither of them under a fallback identity, because the title named both nodes");
}

// **The held pose is the title's own, never a lerp.** This is the failure that would make the
// second tick's lerp a third of the way from the wrong place, and it looks right: the picture still
// moves, at the wrong speed, and nothing reports a fault. The test is three in-between paints in a
// row with the title's own value advancing by 10 each time; if the held copy were the written one,
// the second lerp would be 5.0 rather than 15.0.
void theHeldPoseIsTheTitlesOwnAndNeverALerp() {
    PoseByShader poses;
    std::string refusal;
    poses.offer(kShader, 0, perObjectAt(kObjectOffset), refusal);
    PoseBlend blend(poses);
    std::vector<float> written;
    // **Not `const`:** it is a loop's own value, and the rule for a computed value is an ordinary
    // local -- a `const` here reads as a named constant and is not one.
    for (float at : {0.0f, 10.0f, 20.0f, 30.0f}) {
        std::vector<float> words(64, 0.0f);
        putPose(words, kObjectOffset / 4, at);
        blend.onAssemblyAtOffset(words.data(), words.size(), kObjectOffset, kShader, kNodeA,
                                 at != 0.0f);
        written.push_back(words[kObjectOffset / 4 + 9]);
    }
    // 0 held; then 5 = (0+10)/2; then 15 = (10+20)/2 -- **not** (5+20)/2 = 12.5; then 25 =
    // (20+30)/2.
    check::equal(written[1], 5.0f, "the first lerp is of the two ticks the title drew");
    check::equal(written[2], 15.0f,
                 "the second is of ticks N-1 and N as the title drew them, and not of the first "
                 "lerp: 15 rather than 12.5, so the held value was the title's own");
    check::equal(written[3], 25.0f, "and the third likewise, 25 rather than 20");
}

// **Three refusals, each with a count.** A blend that writes twelve words at a wrong offset
// corrupts a value that is not its own, so each of the three ways this can go wrong leaves the
// title's value in place and says so.
void aBlendThatCannotBeSafeWritesNothingAndSaysWhy() {
    PoseByShader poses;
    std::string refusal;
    poses.offer(kShader, 0, perObjectAt(kObjectOffset), refusal);
    // A pass's value: 12 of 15 objects share it, so the table refuses to place a pose there.
    auto pass = perObjectAt(kPassOffset);
    pass.otherObjectsSame = 12;
    poses.offer(0xb7252004aba21c10, 0, pass, refusal);

    PoseBlend blend(poses);

    // **One: a shader the table has no offset for.** The draw is left exactly as the title wrote
    // it.
    std::vector<float> unseen(64, 7.0f);
    blend.onAssembly(unseen.data(), unseen.size(), 0xdeadbeef, 0, kNodeA, true);
    check::equal(unseen[kObjectOffset / 4 + 9], 7.0f,
                 "a shader the table has never seen is left exactly as the title wrote it");
    check::equal(blend.tally().withoutShader, uint64_t{1}, "and counted");

    // **Two: an offset past the end of this assembly.** Same shader, a shorter buffer -- which is
    // what a different draw of the same shader can hand over.
    std::vector<float> shortBuffer(6, 3.0f);
    blend.onAssemblyAtOffset(shortBuffer.data(), shortBuffer.size(), kObjectOffset, kShader, kNodeA,
                             true);
    check::equal(shortBuffer[0], 3.0f, "an offset past the end of the buffer writes nothing");
    check::equal(
        blend.tally().outOfRange, uint64_t{1},
        "and is counted separately from a shader it had no offset for, because the two are "
        "different faults with different fixes");

    // **Three: a word a lerp may not touch.** A pose holding a denormal cannot be blended, and a
    // partial write would be a matrix with a row from each frame.
    std::vector<float> first(64, 0.0f);
    putPose(first, kObjectOffset / 4, 4.0f);
    first[kObjectOffset / 4 + 5] = 5.0e-40f; // a denormal, in the held pose
    blend.onAssemblyAtOffset(first.data(), first.size(), kObjectOffset, kShader, kNodeB, false);
    std::vector<float> second(64, 0.0f);
    putPose(second, kObjectOffset / 4, 12.0f);
    blend.onAssemblyAtOffset(second.data(), second.size(), kObjectOffset, kShader, kNodeB, true);
    check::equal(second[kObjectOffset / 4 + 9], 12.0f,
                 "the tick's own value is left in place rather than a pose with one row from each "
                 "frame");
    check::equal(blend.tally().refusedUnblendable, uint64_t{1}, "and the refusal is counted");

    // And a pass's value, through the table's own lookup, is refused before anything is written.
    std::vector<float> passWords(64, 1.0f);
    blend.onAssembly(passWords.data(), passWords.size(), 0xb7252004aba21c10, 0, kNodeA, true);
    check::equal(passWords[kPassOffset / 4], 1.0f,
                 "a pass's value is never written, because moving it would move the light with the "
                 "object");
    check::equal(blend.tally().withoutShader, uint64_t{2},
                 "and it is counted as a draw the blend could not place, beside the unseen shader");

    // **Which shaders, counted, most frequent first.** The first run on the real title reported
    // `withoutShader` equal to `assemblies` and no reason at all, and the reason -- that the
    // table's accepted shaders are not the ones the scene draws with -- had to be found by hand. A
    // total says a blend is not happening; a ranked list of the shaders says why.
    const auto unplaced = blend.unplacedShaders();
    check::equal(unplaced.size(), size_t{2}, "two shaders could not place a pose");
    check::equal(unplaced[0].draws, uint64_t{1}, "and both drew once, so the order is by count");
    check::isTrue(unplaced[0].shaderBaseHash == 0xdeadbeef ||
                      unplaced[0].shaderBaseHash == 0xb7252004aba21c10,
                  "the first is one of the two the run refused: " +
                      std::to_string(unplaced[0].shaderBaseHash));
    check::isTrue(!blend.unplacedCapped(), "and the bound was not reached, so the list is whole");
}

// **The identity is the node, and a pair held without one is reported as such.** The title's binder
// names the node; where it has not, the fallback is the shader, which measured nearly unique per
// draw, so those pairs are held per shader rather than per object. A reader has to be able to see
// which identity the held pairs are filed under, because "held 590" means two different things.
void aPairHeldWithoutTheTitlesNodeIsCountedAsAFallback() {
    PoseByShader poses;
    std::string refusal;
    poses.offer(kShader, 0, perObjectAt(kObjectOffset), refusal);
    PoseBlend blend(poses);

    std::vector<float> words(64, 0.0f);
    putPose(words, kObjectOffset / 4, 2.0f);
    // `objectAddress == 0` is "the title has not said which node this is".
    blend.onAssemblyAtOffset(words.data(), words.size(), kObjectOffset, kShader, 0, false);
    check::equal(blend.held(), uint64_t{1}, "the pose is held");
    check::equal(blend.heldOnFallbackIdentity(), uint64_t{1},
                 "and counted as a fallback, so 'held 1' is not read as one node");
    // Two draws of the same shader with no node are *one* pair under the fallback, and that is the
    // weakness: the second draw's pose overwrites the first's rather than being compared with it.
    putPose(words, kObjectOffset / 4, 6.0f);
    blend.onAssemblyAtOffset(words.data(), words.size(), kObjectOffset, kShader, 0, true);
    check::equal(words[kObjectOffset / 4 + 9], 4.0f,
                 "which is the midpoint of the fallback's two readings -- the arithmetic, and the "
                 "limit of the fallback identity, in one number");
    check::equal(blend.held(), uint64_t{1}, "and still one pair, not two");
}

// **A report field that is a constant is worse than one that is absent.** The one number a reader
// needs is held pairs against lerps: a table full of poses and no lerps means a blend that is not
// happening, and no field of the counters says that on its own.
void theReportSaysHeldPairsAgainstLerps() {
    PoseByShader poses;
    std::string refusal;
    poses.offer(kShader, 0, perObjectAt(kObjectOffset), refusal);
    PoseBlend blend(poses);
    // Held, and nothing blended: the ratio must say so rather than reading as a healthy table.
    std::vector<float> words(64, 0.0f);
    putPose(words, kObjectOffset / 4, 1.0f);
    blend.onAssemblyAtOffset(words.data(), words.size(), kObjectOffset, kShader, kNodeA, false);
    check::isTrue(blend.json().find("\"lerpsPerInBetween\":0") != std::string::npos,
                  "a pose held and never blended reads as zero lerps per in-between draw: " +
                      blend.json());
    // And the two counts are named apart, because they were one field once and meant two things.
    check::isTrue(blend.json().find("\"held\":1") != std::string::npos &&
                      blend.json().find("\"refreshed\":1") != std::string::npos,
                  "held is the pairs and refreshed is the assemblies, under their own names: " +
                      blend.json());
    // And a second node that is blended, so the ratio moves off zero.
    std::vector<float> other(64, 0.0f);
    putPose(other, kObjectOffset / 4, 1.0f);
    blend.onAssemblyAtOffset(other.data(), other.size(), kObjectOffset, kShader, kNodeB, false);
    putPose(other, kObjectOffset / 4, 9.0f);
    blend.onAssemblyAtOffset(other.data(), other.size(), kObjectOffset, kShader, kNodeB, true);
    const std::string report = blend.json();
    check::isTrue(report.find("\"lerpsPerInBetween\":100") != std::string::npos,
                  "one lerp over one in-between draw over a pair that was held reads as a hundred "
                  "per cent, which is a blend that is happening: " +
                      report);
    // And the bound is in the report, so a full table is distinguishable from a short one.
    check::isTrue(report.find("\"maxHeld\":") != std::string::npos,
                  "the bound is beside the count: " + report);
}

} // namespace

void wiiuport::tests::runPoseBlendTests() {
    theInBetweenFrameIsTheMidpointOfTheLastTwoTicksAndOnlyOnTheInBetweenPaint();
    theHeldPoseIsTheTitlesOwnAndNeverALerp();
    aBlendThatCannotBeSafeWritesNothingAndSaysWhy();
    aPairHeldWithoutTheTitlesNodeIsCountedAsAFallback();
    theReportSaysHeldPairsAgainstLerps();
}
