#include "check.h"
#include "suites.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/interp/FrameInterpolator.h"
#include "wiiuport/interp/TransformSearch.h"
#include "wiiuport/interp/TransformSubstitution.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using wiiuport::frame::FrameRecording;
using wiiuport::frame::RecordedUniformAssembly;
using wiiuport::interp::FrameInterpolator;
using wiiuport::interp::ShaderKey;
using wiiuport::interp::Transform3x4;
using wiiuport::interp::TransformSearch;
using wiiuport::interp::TransformSubstitution;
using wiiuport::interp::ViewSlot;

namespace {

// A view at a translation, with an identity rotation: what the search accepts
// and what a blend has to reproduce at its endpoints.
std::array<float, 12> viewAt(float x, float y, float z) {
    return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z};
}

// One shader's buffer: `lead` floats of something else, then the view.
std::vector<float> bufferWithView(size_t lead, const std::array<float, 12>& view) {
    std::vector<float> data(lead, 0.5f);
    data.insert(data.end(), view.begin(), view.end());
    return data;
}

void addAssembly(FrameRecording& recording, uint64_t shaderHash, const std::vector<float>& data) {
    RecordedUniformAssembly assembly;
    assembly.shaderBaseHash = shaderHash;
    assembly.data = data;
    recording.addUniformAssembly(assembly);
}

// Two shaders carrying one view at different offsets, moving between frames.
void observeTwoFrames(TransformSearch& search, const std::array<float, 12>& first,
                      const std::array<float, 12>& second) {
    for (const auto& view : {first, second}) {
        FrameRecording frame;
        addAssembly(frame, 0xaaaa, bufferWithView(0, view));
        addAssembly(frame, 0xbbbb, bufferWithView(4, view));
        search.observe(frame);
    }
}

LatteFrameHooks::UniformAssembly assemblyOver(uint64_t shaderHash, std::vector<float>& data) {
    LatteFrameHooks::UniformAssembly assembly{};
    assembly.shaderBaseHash = shaderHash;
    assembly.data = data.data();
    assembly.sizeInBytes = static_cast<uint32_t>(data.size() * sizeof(float));
    assembly.fromRuntime = true;
    return assembly;
}

void oneFrameGivesNoEndpointsToBlendBetween() {
    // A blend between a value and itself is not a blend, and arming one would
    // look exactly like interpolation that ran.
    TransformSearch search;
    FrameRecording frame;
    addAssembly(frame, 0xaaaa, bufferWithView(0, viewAt(0, 0, 0)));
    addAssembly(frame, 0xbbbb, bufferWithView(4, viewAt(0, 0, 0)));
    search.observe(frame);
    check::equal(search.viewSlots().size(), size_t{0}, "one frame yields no slot to blend");
}

void aViewCarriedByTwoShadersIsFoundAtEachOffset() {
    TransformSearch search;
    observeTwoFrames(search, viewAt(0, 0, 0), viewAt(10, 20, 30));
    std::vector<ViewSlot> slots = search.viewSlots();
    check::equal(slots.size(), size_t{2}, "both shaders carrying the view are returned");

    const ViewSlot* wide = nullptr;
    for (const ViewSlot& slot : slots) {
        if (slot.shader.baseHash == 0xbbbb) {
            wide = &slot;
        }
    }
    check::isTrue(wide != nullptr, "including the one whose buffer has a lead-in");
    if (wide == nullptr) {
        return;
    }
    check::equal(wide->floatOffset, uint32_t{4}, "at the offset that shader carries it at");
    check::equal(wide->before.translation().x, 0.0f, "with the frame before last as one endpoint");
    check::equal(wide->after.translation().x, 10.0f, "and the last frame as the other");
}

void aViewOnlyOneShaderCarriesIsNotOfferedForSubstitution() {
    // An object's own transform reaches the shaders that draw that object. A
    // camera reaches every pass that draws the world, and that difference is
    // the only thing separating them here.
    TransformSearch search;
    for (const auto& view : {viewAt(0, 0, 0), viewAt(10, 20, 30)}) {
        FrameRecording frame;
        addAssembly(frame, 0xaaaa, bufferWithView(0, view));
        addAssembly(frame, 0xbbbb, bufferWithView(0, viewAt(1, 1, 1)));
        search.observe(frame);
    }
    check::equal(search.viewSlots().size(), size_t{0},
                 "a transform in one shader alone is not treated as the view");
}

void aBlendIsWrittenIntoTheDrawTheRuntimeReplayed() {
    TransformSearch search;
    observeTwoFrames(search, viewAt(0, 0, 0), viewAt(10, 20, 30));
    TransformSubstitution substitution;
    check::isTrue(substitution.armOnce(search.viewSlots(), 0.5f), "the substitution arms");
    check::equal(substitution.slotCount(), size_t{2}, "holding both slots");

    std::vector<float> data = bufferWithView(4, viewAt(999, 999, 999));
    auto assembly = assemblyOver(0xbbbb, data);
    check::isTrue(substitution.onRuntimeAssembly(assembly), "the draw is substituted");
    check::near(data[4 + 3], 5.0f, 1e-5f, "halfway between the two frames in x");
    check::near(data[4 + 7], 10.0f, 1e-5f, "and in y");
    check::near(data[4 + 11], 15.0f, 1e-5f, "and in z");
    check::equal(data[0], 0.5f, "leaving the rest of the buffer alone");
    check::equal(substitution.assembliesSubstituted(), uint64_t{1}, "and counting the write");
}

void theEndpointsAreExactSoANonBlendCannotLookLikeOne() {
    TransformSearch search;
    observeTwoFrames(search, viewAt(0, 0, 0), viewAt(10, 20, 30));
    TransformSubstitution substitution;
    substitution.armOnce(search.viewSlots(), 1.0f);
    std::vector<float> data = bufferWithView(0, viewAt(999, 999, 999));
    auto assembly = assemblyOver(0xaaaa, data);
    substitution.onRuntimeAssembly(assembly);
    check::equal(data[3], 10.0f, "t=1 writes the last frame's value unchanged");
}

void anUnarmedSubstitutionWritesNothingAndSaysSo() {
    TransformSubstitution substitution;
    std::vector<float> data = bufferWithView(0, viewAt(7, 7, 7));
    auto assembly = assemblyOver(0xaaaa, data);
    check::isTrue(!substitution.onRuntimeAssembly(assembly), "nothing is written unarmed");
    check::equal(data[3], 7.0f, "the buffer is untouched");
    check::equal(substitution.assembliesUnarmed(), uint64_t{1}, "and the reason is counted");
    check::equal(substitution.assembliesOffered(), uint64_t{1}, "against a denominator");
}

void aDrawThatDoesNotCarryTheViewIsCountedNotWritten() {
    TransformSearch search;
    observeTwoFrames(search, viewAt(0, 0, 0), viewAt(10, 20, 30));
    TransformSubstitution substitution;
    substitution.armOnce(search.viewSlots(), 0.5f);

    std::vector<float> data = bufferWithView(0, viewAt(7, 7, 7));
    auto assembly = assemblyOver(0xcccc, data);
    check::isTrue(!substitution.onRuntimeAssembly(assembly), "another shader's draw is left alone");
    check::equal(data[3], 7.0f, "with its buffer unchanged");
    check::equal(substitution.assembliesUnknownShader(), uint64_t{1},
                 "counted as a draw that does not carry the view");
}

void aBufferShorterThanTheOffsetIsRefusedNotOverrun() {
    TransformSearch search;
    observeTwoFrames(search, viewAt(0, 0, 0), viewAt(10, 20, 30));
    TransformSubstitution substitution;
    substitution.armOnce(search.viewSlots(), 0.5f);

    // The view was found at offset 4 in this shader; this draw assembled a
    // buffer with no room for it.
    std::vector<float> data(8, 0.25f);
    auto assembly = assemblyOver(0xbbbb, data);
    check::isTrue(!substitution.onRuntimeAssembly(assembly), "the short buffer is refused");
    check::equal(data.back(), 0.25f, "and nothing was written past its end");
    check::equal(substitution.assembliesTooShort(), uint64_t{1}, "counted as too short");
}

bool acceptSubmission(const void*, uint32_t) {
    return true;
}

bool refuseCapture(LatteFrameHooks::CaptureCallback) {
    return false;
}

bool refusePresent(const LatteFrameHooks::PresentArguments&) {
    return false;
}

// One interpolator over the real scheduler it arms, because what it refuses
// depends on what the scheduler already has in flight.
struct InterpolatorFixture {
    TransformSearch search;
    TransformSubstitution substitution;
    wiiuport::frame::FrameReplayer replayer{&acceptSubmission};
    wiiuport::frame::FramePresenter presenter{&refusePresent};
    wiiuport::frame::FrameCapture capture{&refuseCapture};
    wiiuport::frame::ReplayScheduler scheduler{replayer, presenter, capture};
    FrameInterpolator interpolator{search, substitution, scheduler};
};

void anInterpolatedFrameArmsBothTheBlendAndTheReplay() {
    InterpolatorFixture fixture;
    observeTwoFrames(fixture.search, viewAt(0, 0, 0), viewAt(10, 20, 30));
    check::isTrue(fixture.interpolator.armOnce(0.5f), "the frame arms");
    check::isTrue(fixture.substitution.isArmed(), "with the blend armed");
    check::isTrue(fixture.scheduler.nullDiffPending(), "and a captured pair in flight");
    check::equal(fixture.interpolator.lastRefusal(), std::string(), "and nothing refused");
}

void withNoViewFoundTheRefusalSaysSoRatherThanArmingNothing() {
    // The failure this guards against is an interpolated frame that arms,
    // replays, substitutes nothing, and comes out identical to the title's
    // frame -- which reads exactly like interpolation that does not work.
    InterpolatorFixture fixture;
    check::isTrue(!fixture.interpolator.armOnce(0.5f), "arming is refused");
    check::equal(fixture.interpolator.lastRefusal(), std::string("no view transform found yet"),
                 "and names the reason");
    check::isTrue(!fixture.scheduler.nullDiffPending(), "with no capture left in flight");
    check::equal(fixture.interpolator.framesRefused(), uint64_t{1}, "counted as a refusal");
}

void aBlendPointOutsideTheTwoFramesIsRefused() {
    InterpolatorFixture fixture;
    observeTwoFrames(fixture.search, viewAt(0, 0, 0), viewAt(10, 20, 30));
    check::isTrue(!fixture.interpolator.armOnce(1.5f), "past the last frame is refused");
    check::isTrue(!fixture.interpolator.armOnce(std::nanf("")), "and so is a NaN");
    check::isTrue(!fixture.substitution.isArmed(), "with nothing armed either time");
}

void aSecondInterpolatedFrameDoesNotTakeOverTheFirst() {
    InterpolatorFixture fixture;
    observeTwoFrames(fixture.search, viewAt(0, 0, 0), viewAt(10, 20, 30));
    fixture.interpolator.armOnce(0.5f);
    check::isTrue(!fixture.interpolator.armOnce(0.25f), "the second is refused");
    check::equal(fixture.interpolator.lastRefusal(), std::string("a replay is already in flight"),
                 "naming the frame already in flight");
    check::near(fixture.substitution.blendPoint(), 0.5f, 1e-6f,
                "and the first frame's blend is left as it was");
}

void theBlendIsTakenDownOnceItsFrameHasBeenReplayed() {
    // Two frame ends: the first arms the pair of captures, the second runs
    // the replay. After that the blend belongs to a frame that is over.
    InterpolatorFixture fixture;
    observeTwoFrames(fixture.search, viewAt(0, 0, 0), viewAt(10, 20, 30));
    fixture.interpolator.armOnce(0.5f);
    FrameRecording frame;
    addAssembly(frame, 0xaaaa, bufferWithView(0, viewAt(10, 20, 30)));
    for (int end = 0; end < 2; ++end) {
        // The order the recorder calls them in.
        fixture.scheduler.onFrameRecorded(frame);
        fixture.interpolator.onFrameRecorded(frame);
    }
    check::isTrue(!fixture.substitution.isArmed(), "the blend is disarmed after its replay");
    check::isTrue(fixture.substitution.assembliesSubstituted() == 0,
                  "and this fixture's replay never reached a runtime assembly");
}

void armingWithNothingToSubstituteIsRefused() {
    TransformSubstitution substitution;
    check::isTrue(!substitution.armOnce({}, 0.5f), "an empty slot list does not arm");
    check::isTrue(!substitution.isArmed(), "so nothing claims to be interpolating");
}

} // namespace

namespace wiiuport::tests {

void runSubstitutionTests() {
    oneFrameGivesNoEndpointsToBlendBetween();
    aViewCarriedByTwoShadersIsFoundAtEachOffset();
    aViewOnlyOneShaderCarriesIsNotOfferedForSubstitution();
    aBlendIsWrittenIntoTheDrawTheRuntimeReplayed();
    theEndpointsAreExactSoANonBlendCannotLookLikeOne();
    anUnarmedSubstitutionWritesNothingAndSaysSo();
    aDrawThatDoesNotCarryTheViewIsCountedNotWritten();
    aBufferShorterThanTheOffsetIsRefusedNotOverrun();
    armingWithNothingToSubstituteIsRefused();
    anInterpolatedFrameArmsBothTheBlendAndTheReplay();
    withNoViewFoundTheRefusalSaysSoRatherThanArmingNothing();
    aBlendPointOutsideTheTwoFramesIsRefused();
    aSecondInterpolatedFrameDoesNotTakeOverTheFirst();
    theBlendIsTakenDownOnceItsFrameHasBeenReplayed();
}

} // namespace wiiuport::tests
