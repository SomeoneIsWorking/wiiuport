#include "wiiuport/frame/FrameShapeLog.h"

#include <set>

namespace wiiuport::frame {
namespace {

size_t countDistinctShaders(const FrameRecording& recording) {
    std::set<std::tuple<uint64_t, uint64_t, uint32_t>> shaders;
    for (const RecordedUniformAssembly& assembly : recording.uniformAssemblies()) {
        shaders.emplace(assembly.shaderBaseHash, assembly.shaderAuxHash, assembly.stageIndex);
    }
    return shaders.size();
}

} // namespace

FrameShapeLog::FrameShapeLog(size_t depth) : m_depth(depth == 0 ? 1 : depth) {
}

void FrameShapeLog::onFrameRecorded(const FrameRecording& recording) {
    m_shapes.push_back(FrameShape{
        m_framesLogged, recording.displayLists().size(), recording.uniformAssemblies().size(),
        countDistinctShaders(recording), recording.byteCount(), recording.isComplete()});
    ++m_framesLogged;
    while (m_shapes.size() > m_depth) {
        m_shapes.pop_front();
    }
}

} // namespace wiiuport::frame
