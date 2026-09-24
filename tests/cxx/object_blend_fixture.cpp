#include "object_blend_fixture.h"

using wiiuport::frame::FrameRecording;
using wiiuport::frame::RecordedUniformAssembly;
using wiiuport::interp::ObjectBlend;

namespace wiiuport::tests::object_blend {

std::vector<uint32_t> Draw::sources() const {
    std::vector<uint32_t> words = {1, block};
    if (freshBlock != 0) {
        words.insert(words.end(), {2, freshBlock});
    }
    if (passBlock != 0) {
        words.insert(words.end(), {3, passBlock});
    }
    return words;
}

ReplayedDraw::ReplayedDraw(const Draw& draw) : sources(draw.sources()), values(draw.values) {
    assembly.shaderBaseHash = draw.shader;
    assembly.stageIndex = draw.stage;
    assembly.writesColour = draw.writesColour;
    assembly.looksUpDepthMap = draw.looksUpDepthMap;
    assembly.data = values.data();
    assembly.sizeInBytes = static_cast<uint32_t>(values.size() * sizeof(float));
    assembly.blockAddresses = sources.data();
    assembly.blockAddressCount = static_cast<uint32_t>(sources.size() / 2);
    assembly.fromRuntime = true;
}

FrameRecording frameOf(const std::vector<Draw>& draws) {
    FrameRecording frame;
    for (const Draw& draw : draws) {
        RecordedUniformAssembly assembly;
        assembly.shaderBaseHash = draw.shader;
        assembly.stageIndex = draw.stage;
        assembly.writesColour = draw.writesColour;
        assembly.looksUpDepthMap = draw.looksUpDepthMap;
        assembly.blockSources = draw.sources();
        assembly.data = draw.values;
        frame.addUniformAssembly(assembly);
    }
    return frame;
}

std::vector<std::vector<float>> replay(ObjectBlend& blend, const std::vector<Draw>& frame) {
    std::vector<std::vector<float>> uploaded;
    for (const Draw& draw : frame) {
        ReplayedDraw replayed(draw);
        blend.apply(replayed.assembly);
        uploaded.push_back(replayed.values);
    }
    return uploaded;
}

void record(ObjectBlend& blend, const std::vector<Draw>& draws) {
    FrameRecording frame = frameOf(draws);
    for (const RecordedUniformAssembly& assembly : frame.uniformAssemblies()) {
        blend.onAssemblyRecorded(assembly);
    }
    blend.onFrameRecorded(frame);
}

void armAfter(ObjectBlend& blend, const std::vector<Draw>& twoBack,
              const std::vector<Draw>& oneBack, const std::vector<Draw>& latest) {
    blend.setPlanning(true);
    record(blend, twoBack);
    record(blend, oneBack);
    record(blend, latest);
    blend.armOnce();
}

std::vector<Draw> walkTwoBack() {
    return {{kBlockA, {0.0f, 7.0f}}, {kOtherA, {5.0f, 5.0f}}};
}

std::vector<Draw> walkOneBack() {
    return {{kBlockB, {1.0f, 7.0f}}, {kOtherB, {5.0f, 5.0f}}};
}

std::vector<Draw> walkLatest() {
    return {{kBlockA, {2.0f, 7.0f}}, {kOtherA, {5.0f, 5.0f}}};
}

} // namespace wiiuport::tests::object_blend
