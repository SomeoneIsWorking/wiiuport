#include "check.h"
#include "suites.h"
#include "wiiuport/interp/Blendable.h"
#include "wiiuport/title/PoseByShader.h"

#include <array>
#include <cmath>
#include <string>

using wiiuport::title::PoseByShader;

namespace {

// The two shaders and the offsets the real title measured, and the pass's value at a third.
constexpr uint64_t kObjectShaderA = 0x1557c18f92f3bcb9;
constexpr uint64_t kObjectShaderB = 0x8cecd19741c6c1c7;
constexpr uint64_t kPassShader = 0xb7252004aba21c10;

PoseByShader::Entry perObject(uint32_t byteOffset, uint64_t moved, uint64_t compared) {
    PoseByShader::Entry entry;
    entry.byteOffset = byteOffset;
    entry.moved = moved;
    entry.compared = compared;
    entry.otherObjects = 15;
    entry.otherObjectsSame = 0;
    return entry;
}

} // namespace

// **The table the measurement says the title has, and the two refusals that keep a pass's value and
// a basis matrix out of a blend's way.** A blend that wrote twelve words at a wrong offset would
// corrupt a value that is not its own, so every refusal is counted and named, and the negative is
// beside the positive: a shader the table has not seen, a value most objects share, and a value
// that never moves are three different facts and each must read as itself.
void aPerObjectPoseIsFoundByItsShaderAndTheOtherTwoAreRefused() {
    PoseByShader poses;
    std::string refusal;

    // The two per-object entries, at the offsets and with the counts the title measured.
    poses.offer(kObjectShaderA, 0, perObject(12, 21730, 21879), refusal);
    check::isTrue(refusal.empty(), "the per-object pose at offset 12 is offered: " + refusal);
    poses.offer(kObjectShaderB, 0, perObject(4, 202, 21950), refusal);
    check::isTrue(refusal.empty(), "and the same pose at offset 4 in the other shader: " + refusal);

    // A pass's view projection: shared by 12 of 15 objects. **Refused, and the count is in the
    // reason** -- a blend that wrote a pass's camera at t would move the light with the object.
    auto pass = perObject(76, 98, 235);
    pass.otherObjectsSame = 12;
    poses.offer(kPassShader, 0, pass, refusal);
    check::isTrue(refusal.find("pass's value") != std::string::npos,
                  "a value 12 of 15 objects share is a pass's and is refused: " + refusal);
    check::isTrue(refusal.find("12") != std::string::npos &&
                      refusal.find("15") != std::string::npos,
                  "and the refusal carries both counts, so a reader can see how near the bar it "
                  "was: " +
                      refusal);

    // A basis matrix: shaped like a transform and never changes. Refused, with its own count.
    auto still = perObject(0, 0, 193570);
    poses.offer(0x44f85a8fe341045c, 0, still, refusal);
    check::isTrue(refusal.find("basis matrix") != std::string::npos,
                  "a value that never moved is a basis matrix and is refused: " + refusal);
    check::isTrue(refusal.find("193570") != std::string::npos,
                  "with the count that said so: " + refusal);

    // A shader nothing was said about: refused, and the refusal says the draw is left alone rather
    // than placed at some other shader's offset.
    auto looked = poses.offsetFor(0xdeadbeef, 0, refusal);
    check::isTrue(!looked.has_value(), "an unseen shader has no offset");
    check::isTrue(refusal.find("left alone") != std::string::npos,
                  "and the refusal says the draw is left alone rather than placed somewhere: " +
                      refusal);

    // And the two that were offered are found, at the offsets the title measured.
    auto first = poses.offsetFor(kObjectShaderA, 0, refusal);
    check::isTrue(first.has_value() && *first == 12, "shader A's pose is at offset 12");
    check::isTrue(refusal.empty(), "with nothing refused about it");
    auto second = poses.offsetFor(kObjectShaderB, 0, refusal);
    check::isTrue(second.has_value() && *second == 4, "and shader B's at offset 4");
    check::isTrue(!poses.offsetFor(kPassShader, 0, refusal).has_value(),
                  "while the pass's shader is still refused, having never been given one");
}

// **The key is the pair.** Two shaders sharing a base hash are two shaders, and a table that
// conflated them would place one shader's draw at the other's offset -- which is the same fault the
// per-shader denominator had, where one "shader" of 45,475 assemblies was three.
void theKeyIsTheShaderAndNotItsBaseHashAlone() {
    PoseByShader poses;
    std::string refusal;
    poses.offer(0xdddd, 1, perObject(12, 100, 100), refusal);
    check::isTrue(refusal.empty(), "the first shader is offered: " + refusal);
    // A second with the same base and a different aux: refused as a duplicate, which is how the two
    // are told apart rather than one overwriting the other.
    poses.offer(0xdddd, 2, perObject(4, 50, 50), refusal);
    check::isTrue(refusal.empty(),
                  "and the second, with the same base hash, is a different one: " + refusal);
    check::isTrue(poses.offsetFor(0xdddd, 1, refusal).value_or(0) == 12,
                  "so the first still resolves to offset 12");
    check::isTrue(poses.offsetFor(0xdddd, 2, refusal).value_or(0) == 4,
                  "and the second to offset 4");
}

// **A value a blend may not touch is a value it is told about, not one it assumes.** A colour
// triple and a packed integer both satisfy a shape test, and averaging two of either is not a
// matrix.
void aValueThatCannotBeBlendedIsCountedRatherThanAssumed() {
    const std::array<float, PoseByShader::kWords> plain{1.0f, 0.0f, -1.0f, 0.5f, 0.0f, 2.0f,
                                                        0.0f, 0.0f, 1.0f,  3.0f, 0.0f, -4.0f};
    check::equal(PoseByShader::blendableWords(plain.data()), uint32_t{PoseByShader::kWords},
                 "a pose of zeros and normal floats is twelve blendable words");
    const std::array<float, PoseByShader::kWords> withDenormal{
        1.0f, 0.0f, -1.0f, 5e-40f, 0.0f, 2.0f, 0.0f, 0.0f, 1.0f, 3.0f, 0.0f, -4.0f};
    check::equal(PoseByShader::blendableWords(withDenormal.data()), uint32_t{11},
                 "and a denormal is not, because averaging two of them makes an integer neither "
                 "frame wrote");
    const std::array<float, PoseByShader::kWords> withInfinity{
        std::numeric_limits<float>::infinity(),
        0.0f,
        -1.0f,
        0.5f,
        0.0f,
        2.0f,
        0.0f,
        0.0f,
        1.0f,
        3.0f,
        0.0f,
        -4.0f};
    check::equal(
        PoseByShader::blendableWords(withInfinity.data()), uint32_t{11},
        "and an infinity is not either, because a lerp towards one is a number nothing can "
        "draw");
    // And the one implementation of the rule, asked rather than defined: this class's predicate and
    // `Blendable`'s must agree word for word, because two copies of a rule that decides what a
    // blend may touch is one more thing to fall out of step.
    uint32_t byBlendable = 0;
    for (const float word : plain) {
        if (wiiuport::interp::isNumber(word)) {
            ++byBlendable;
        }
    }
    check::equal(PoseByShader::blendableWords(plain.data()), byBlendable,
                 "and the class asks Blendable's rule rather than keeping a second copy of it");
}

void wiiuport::tests::runPoseByShaderTests() {
    aPerObjectPoseIsFoundByItsShaderAndTheOtherTwoAreRefused();
    theKeyIsTheShaderAndNotItsBaseHashAlone();
    aValueThatCannotBeBlendedIsCountedRatherThanAssumed();
}
