#include "wiiuport/frame/RecordingObserver.h"

#include <algorithm>
#include <utility>

namespace wiiuport::frame {

void RecordingObserver::OnDisplayList(const LatteFrameHooks::DisplayList& list) {
    m_displayListsSeen++;
    if (list.fromRuntime) {
        // A replay's own nested buffers. Recording them would make the next
        // frame a copy of this one with the replay folded in.
        ++m_displayListsFromRuntime;
        return;
    }
    if (!list.topLevel) {
        // Reached by walking the buffer that referenced it, so replaying the
        // top-level buffer draws it again. Recording it as well would issue
        // its contents twice.
        ++m_nestedListsSeen;
        return;
    }
    m_inFlight.addDisplayList(list.physicalAddress, list.data, list.sizeInBytes);
}

void RecordingObserver::OnUniformAssembly(const LatteFrameHooks::UniformAssembly& assembly) {
    m_uniformAssembliesSeen++;
    if (assembly.fromRuntime) {
        ++m_uniformAssembliesFromRuntime;
        // The one moment a substitution belongs to: the runtime's own draw,
        // after the renderer assembled the buffer and before it uploads it.
        if (m_assemblyFilter != nullptr) {
            m_assemblyFilter->onRuntimeAssembly(assembly);
        }
        return;
    }
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

void RecordingObserver::OnPresent(const LatteFrameHooks::PresentArguments& present) {
    ++m_presentsSeen;
    for (PresentListener* listener : m_presentListeners) {
        listener->onPresentObserved(present);
    }
}

void RecordingObserver::OnRuntimeSubmission(const LatteFrameHooks::SubmissionSummary& summary) {
    ++m_runtimeSubmissions;
    m_runtimePacketsProcessed += summary.packetsProcessed;
    m_runtimeDrawsIssued += summary.drawsIssued;
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
