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

std::span<const uint32_t> sourceWordsOf(const LatteFrameHooks::UniformAssembly& assembly) {
    if (assembly.blockAddresses == nullptr) {
        return {};
    }
    uint32_t pairs = std::min(assembly.blockAddressCount, LatteFrameHooks::kMaxUniformBlockSources);
    return {assembly.blockAddresses, static_cast<size_t>(pairs) * 2};
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
    // Filled in place, so its vectors keep their storage from draw to draw.
    RecordedUniformAssembly& recorded = m_assemblyScratch;
    recorded.shaderBaseHash = assembly.shaderBaseHash;
    recorded.shaderAuxHash = assembly.shaderAuxHash;
    recorded.stageIndex = assembly.stageIndex;
    recorded.writesColour = assembly.writesColour;
    std::span<const uint32_t> sources = sourceWordsOf(assembly);
    recorded.blockSources.assign(sources.begin(), sources.end());
    recorded.data.assign(assembly.data, assembly.data + assembly.sizeInBytes / sizeof(float));
    if (!m_inFlight.addUniformAssembly(recorded)) {
        return;
    }
    for (AssemblyRecordedListener* listener : m_assemblyListeners) {
        listener->onAssemblyRecorded(recorded);
    }
}

void RecordingObserver::OnPresent(const LatteFrameHooks::PresentArguments& present) {
    ++m_presentsSeen;
    for (PresentListener* listener : m_presentListeners) {
        listener->onPresentObserved(present);
    }
}

void RecordingObserver::OnGuestDraw(bool fromCommandBuffer) {
    if (fromCommandBuffer) {
        ++m_guestDrawsFromCommandBuffers;
        return;
    }
    ++m_guestDrawsFromRing;
}

void RecordingObserver::OnDrawPrepared(const LatteFrameHooks::DrawPrepared& draw,
                                       LatteFrameHooks::VertexReplacements& replacements) {
    if (draw.fromRuntime) {
        if (!draw.vertexReplaceable) {
            return;
        }
        ++m_runtimeDrawsReplaceable;
        if (m_vertexFilter != nullptr && m_vertexFilter->onRuntimeDraw(draw, replacements)) {
            ++m_runtimeDrawsReplaced;
        }
        return;
    }
    ++m_guestDrawsPrepared;
    if (!draw.vertexUniforms) {
        ++m_guestDrawsWithoutVertexUniforms;
    }
    m_vertexChanges.onDraw(draw);
    for (DrawRecordedListener* listener : m_drawListeners) {
        listener->onDrawRecorded(draw);
    }
}

void RecordingObserver::OnRuntimeSubmission(const LatteFrameHooks::SubmissionSummary& summary) {
    ++m_runtimeSubmissions;
    m_runtimePacketsProcessed += summary.packetsProcessed;
    m_runtimeDrawsIssued += summary.drawsIssued;
    for (size_t effect = 0; effect < m_runtimeWithheld.size(); ++effect) {
        m_runtimeWithheld[effect] += summary.withheld[effect];
    }
}

void RecordingObserver::OnFrameComplete() {
    m_framesObserved++;
    m_vertexChanges.onFrameComplete();
    // An incomplete frame is published as incomplete rather than skipped:
    // skipping would leave the frame before it standing in for it, and a
    // listener would act on a frame that is over. Every reader refuses an
    // incomplete recording by asking it.
    if (!m_inFlight.isComplete()) {
        m_framesRefusedIncomplete++;
    }
    // Rotated, so the frame to be filled reuses the oldest one's storage.
    std::swap(m_previous, m_completed);
    std::swap(m_completed, m_inFlight);
    m_inFlight.clear();
    // After publishing, never before: a listener must not be handed a frame
    // that is still being filled.
    for (auto* listener : m_listeners) {
        listener->onFrameRecorded(m_completed);
    }
}

void RecordingObserver::OnDisplayed(bool fromRuntime) {
    for (auto* listener : m_displayedListeners) {
        listener->onDisplayed(fromRuntime);
    }
}

void RecordingObserver::OnShown(const LatteFrameHooks::ShownFrame& shown) {
    for (auto* listener : m_scanOutListeners) {
        listener->onScannedOut(shown);
    }
}

void RecordingObserver::OnFrameEnd() {
    for (auto* listener : m_shownListeners) {
        listener->onFrameShown(m_completed);
    }
}

} // namespace wiiuport::frame
