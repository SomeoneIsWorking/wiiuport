#include "check.h"
#include "suites.h"
#include "wiiuport/interp/TransformSearch.h"

#include <vector>

namespace wiiuport::tests {
namespace {

using frame::FrameRecording;
using frame::RecordedUniformAssembly;
using interp::SearchReport;
using interp::TransformSearch;

constexpr uint64_t kShaderA = 0x1111;
constexpr uint64_t kShaderB = 0x2222;

// A real rotation: identity turned into nothing, with a translation that the
// caller moves. Anything that is not orthonormal must be rejected, so the
// tests build from a matrix that is.
std::vector<float> identityAt(float x, float y, float z) {
    return {1.0f, 0.0f, 0.0f, x, 0.0f, 1.0f, 0.0f, y, 0.0f, 0.0f, 1.0f, z};
}

RecordedUniformAssembly draw(uint64_t base, std::vector<float> data) {
    RecordedUniformAssembly assembly;
    assembly.shaderBaseHash = base;
    assembly.shaderAuxHash = 0;
    assembly.stageIndex = 0;
    assembly.data = std::move(data);
    return assembly;
}

FrameRecording frameOf(const std::vector<RecordedUniformAssembly>& draws) {
    FrameRecording recording;
    for (const auto& one : draws) {
        recording.addUniformAssembly(one);
    }
    return recording;
}

void oneFrameFindsNothing() {
    TransformSearch search;
    search.observe(frameOf({draw(kShaderA, identityAt(1.0f, 2.0f, 3.0f))}));
    auto report = search.search();
    // With one frame nothing is known to change, so a constant and a camera
    // are indistinguishable and neither may be reported.
    check::equal(report.candidates.size(), size_t{0}, "one frame yields no candidate");
    check::equal(report.spansExamined, size_t{0}, "and says so with a denominator");
    check::equal(report.framesObserved, uint32_t{1}, "having still watched the frame");
}

void aMovingSharedTransformIsFound() {
    TransformSearch search;
    search.observe(frameOf({draw(kShaderA, identityAt(1.0f, 0.0f, 0.0f)),
                            draw(kShaderB, identityAt(1.0f, 0.0f, 0.0f))}));
    search.observe(frameOf({draw(kShaderA, identityAt(4.0f, 0.0f, 0.0f)),
                            draw(kShaderB, identityAt(4.0f, 0.0f, 0.0f))}));
    auto report = search.search();
    check::equal(report.candidates.size(), size_t{2}, "both shaders yield a candidate");
    check::isTrue(report.candidates[0].isShared(), "the best is shared");
    check::equal(report.candidates[0].shadersSharing, uint32_t{2}, "by both shaders");
    check::near(report.candidates[0].meanTranslationStep, 3.0f, 1e-4f,
                "and moved the measured distance");
}

void aConstantIsNotACamera() {
    TransformSearch search;
    // Identical in both frames: a baked-in matrix, not a view.
    search.observe(frameOf({draw(kShaderA, identityAt(1.0f, 2.0f, 3.0f))}));
    search.observe(frameOf({draw(kShaderA, identityAt(1.0f, 2.0f, 3.0f))}));
    auto report = search.search();
    check::equal(report.candidates.size(), size_t{0}, "a never-changing matrix is rejected");
    check::isTrue(report.rejectedNeverChanging > 0, "and counted as such");
}

void aPerDrawTransformIsNotACamera() {
    TransformSearch search;
    // Two draws of one shader disagree within the frame, so this is an
    // object's own transform however well it rotates.
    for (auto x : {0.0f, 5.0f}) {
        search.observe(frameOf({draw(kShaderA, identityAt(x, 0.0f, 0.0f)),
                                draw(kShaderA, identityAt(x + 1.0f, 0.0f, 0.0f))}));
    }
    auto report = search.search();
    check::equal(report.candidates.size(), size_t{0}, "a per-draw matrix is rejected");
    check::isTrue(report.rejectedVaryingWithinFrame > 0, "and counted as such");
}

void aNonRotationIsRejected() {
    TransformSearch search;
    // Rows that are not unit length: a colour or projection row, not a view.
    search.observe(frameOf({draw(
        kShaderA, {2.0f, 0.0f, 0.0f, 1.0f, 0.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f, 4.0f, 0.0f})}));
    search.observe(frameOf({draw(
        kShaderA, {2.0f, 0.0f, 0.0f, 9.0f, 0.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f, 4.0f, 0.0f})}));
    auto report = search.search();
    check::equal(report.candidates.size(), size_t{0}, "a non-rotation is rejected");
    check::isTrue(report.rejectedRotation > 0, "and counted as such");
    check::isTrue(report.spansExamined > 0, "having actually examined it");
}

void anIncompleteFrameIsNotFolded() {
    TransformSearch search;
    FrameRecording tiny(16);
    RecordedUniformAssembly big;
    big.shaderBaseHash = kShaderA;
    big.data = identityAt(1.0f, 1.0f, 1.0f);
    // Refused over budget, so the frame is missing draws and a slot that
    // looks constant may only look that way because its draw was dropped.
    check::isTrue(!tiny.addUniformAssembly(big), "the frame refused the draw");
    check::isTrue(!tiny.isComplete(), "so it is incomplete");
    search.observe(tiny);
    check::equal(search.framesObserved(), uint32_t{0}, "and was not folded in");
}

} // namespace

void runSearchTests() {
    oneFrameFindsNothing();
    aMovingSharedTransformIsFound();
    aConstantIsNotACamera();
    aPerDrawTransformIsNotACamera();
    aNonRotationIsRejected();
    anIncompleteFrameIsNotFolded();
}

} // namespace wiiuport::tests
