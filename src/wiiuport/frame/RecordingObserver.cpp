#include "wiiuport/frame/RecordingObserver.h"

#include <algorithm>
#include <utility>

namespace wiiuport::frame {

void RecordingObserver::OnDisplayList(const LatteFrameHooks::DisplayList& list) {
    m_displayListsSeen++;
    m_inFlight.addDisplayList(list.physicalAddress, list.data, list.sizeInBytes);
}

void RecordingObserver::OnUniformAssembly(const LatteFrameHooks::UniformAssembly& assembly) {
    m_uniformAssembliesSeen++;
    RecordedUniformAssembly recorded;
    recorded.shaderBaseHash = assembly.shaderBaseHash;
    recorded.shaderAuxHash = assembly.shaderAuxHash;
    recorded.stageIndex = assembly.stageIndex;
    uint32_t sources =
        std::min(assembly.blockAddressCount, LatteFrameHooks::kMaxUniformBlockSources);
    recorded.blockSources.assign(assembly.blockAddresses, assembly.blockAddresses + sources);
    recorded.data.assign(assembly.data, assembly.data + assembly.sizeInBytes / sizeof(float));
    m_inFlight.addUniformAssembly(recorded);
}

void RecordingObserver::OnFrameEnd() {
    m_framesObserved++;
    // An incomplete frame is dropped rather than published. Replaying one
    // produces an image missing whatever went over the budget, which looks
    // like a rendering bug rather than like the recording failure it is.
    if (m_inFlight.isComplete()) {
        m_completed = std::move(m_inFlight);
    } else {
        m_framesRefusedIncomplete++;
    }
    m_inFlight.clear();
    // After publishing, never before: a listener must not be handed a frame
    // that is still being filled.
    for (auto* listener : m_listeners) {
        listener->onFrameRecorded(m_completed);
    }
}

} // namespace wiiuport::frame
