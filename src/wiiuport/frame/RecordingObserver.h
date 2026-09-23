#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/frame/VertexChanges.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wiiuport::frame {

// The (bufferId, address) words of the blocks one assembly sourced, capped at
// the interface's own limit: a count the interface says cannot happen must not
// be read past the caller's buffer.
std::span<const uint32_t> sourceWordsOf(const LatteFrameHooks::UniformAssembly& assembly);

// Notified once a frame is complete and published, which is the only moment
// anything may act on a whole frame. That is when the guest has finished
// drawing it and before its swap shows it, so a listener here can still put a
// frame of its own on screen ahead of the guest's. Kept as a narrow interface
// so the recorder does not acquire an opinion about what happens next.
class FrameEndListener {
  public:
    virtual ~FrameEndListener() = default;
    virtual void onFrameRecorded(const FrameRecording& recording) = 0;
};

// Notified for each of the guest's uniform assemblies as it is recorded, while
// the frame is still being drawn. Work done here is spread over the frame; work
// done at the frame's end lands between the frame and its swap, where the title
// waits for it.
class AssemblyRecordedListener {
  public:
    virtual ~AssemblyRecordedListener() = default;
    virtual void onAssemblyRecorded(const RecordedUniformAssembly& assembly) = 0;
};

// Notified after the guest's swap has shown the frame most recently recorded.
// Separate from FrameEndListener because some measurements are defined by
// what is already on screen: a null diff captures the guest's present and
// then redraws over it, which only means something once that present has
// happened.
class FrameShownListener {
  public:
    virtual ~FrameShownListener() = default;
    virtual void onFrameShown(const FrameRecording& recording) = 0;
};

// Notified for every present the title makes. Separate from FrameEndListener
// because a present and a finished recording are different moments: the
// arguments arrive with the copy packet, before the swap.
class PresentListener {
  public:
    virtual ~PresentListener() = default;
    virtual void onPresentObserved(const LatteFrameHooks::PresentArguments& present) = 0;
};

// Notified for every frame handed to the display, the guest's and the
// runtime's, once the renderer has presented it.
class DisplayedListener {
  public:
    virtual ~DisplayedListener() = default;
    virtual void onDisplayed(bool fromRuntime) = 0;
};

// Notified when a frame handed to the display reached the screen, as the
// presentation engine reports it, a few frames later; only where the surface
// reports it.
class ScanOutListener {
  public:
    virtual ~ScanOutListener() = default;
    virtual void onScannedOut(const LatteFrameHooks::ShownFrame& shown) = 0;
};

// Something that may edit a uniform buffer the runtime is about to upload.
// Only the runtime's own replayed draws are offered: editing the guest's
// frame would change what the title is showing rather than what the runtime
// is interpolating.
class AssemblyFilter {
  public:
    virtual ~AssemblyFilter() = default;

    // Returns whether it wrote anything, which the recorder counts. The
    // buffer is the renderer's, and this is the last moment before upload.
    virtual bool onRuntimeAssembly(const LatteFrameHooks::UniformAssembly& assembly) = 0;
};

// Notified for each of the guest's draws once it is prepared, in the order it
// drew them, between the uniform assemblies it was drawn with and the next
// draw's. The draw's vertex bytes are the guest's and last only the call.
class DrawRecordedListener {
  public:
    virtual ~DrawRecordedListener() = default;
    virtual void onDrawRecorded(const LatteFrameHooks::DrawPrepared& draw) = 0;
};

// Something that may give a runtime draw vertex bytes of its own. As with
// AssemblyFilter, only the runtime's replayed draws are offered.
class VertexFilter {
  public:
    virtual ~VertexFilter() = default;

    // Returns whether it replaced anything, which the recorder counts. The
    // replacements must outlive the call only until it returns.
    virtual bool onRuntimeDraw(const LatteFrameHooks::DrawPrepared& draw,
                               LatteFrameHooks::VertexReplacements& replacements) = 0;
};

// Fills a FrameRecording from the fork's hooks, and nothing else.
//
// Kept separate from FrameRecording so the recording stays testable without
// the emulator, and kept thin because everything it does is translation: the
// hook types are plain data, the recording owns the copies, and no policy
// about what to record or when to replay lives here.
class RecordingObserver final : public LatteFrameHooks::Observer {
  public:
    RecordingObserver() = default;

    void OnDisplayList(const LatteFrameHooks::DisplayList& list) override;
    void OnUniformAssembly(const LatteFrameHooks::UniformAssembly& assembly) override;
    void OnPresent(const LatteFrameHooks::PresentArguments& present) override;
    void OnFrameComplete() override;
    void OnFrameEnd() override;
    void OnDisplayed(bool fromRuntime) override;
    void OnShown(const LatteFrameHooks::ShownFrame& shown) override;
    void OnGuestDraw(bool fromCommandBuffer) override;
    void OnDrawPrepared(const LatteFrameHooks::DrawPrepared& draw,
                        LatteFrameHooks::VertexReplacements& replacements) override;
    void OnRuntimeSubmission(const LatteFrameHooks::SubmissionSummary& summary) override;

    // Empty by default, so a build that installs no listener behaves as a
    // pure recorder. More than one thing acts on a published frame -- replay
    // and transform search at least -- and each is notified in the order it
    // was added.
    void addFrameEndListener(FrameEndListener* listener) {
        if (listener != nullptr) {
            m_listeners.push_back(listener);
        }
    }

    void addAssemblyRecordedListener(AssemblyRecordedListener* listener) {
        if (listener != nullptr) {
            m_assemblyListeners.push_back(listener);
        }
    }

    void addFrameShownListener(FrameShownListener* listener) {
        if (listener != nullptr) {
            m_shownListeners.push_back(listener);
        }
    }

    void addPresentListener(PresentListener* listener) {
        if (listener != nullptr) {
            m_presentListeners.push_back(listener);
        }
    }

    void addDisplayedListener(DisplayedListener* listener) {
        if (listener != nullptr) {
            m_displayedListeners.push_back(listener);
        }
    }

    void addScanOutListener(ScanOutListener* listener) {
        if (listener != nullptr) {
            m_scanOutListeners.push_back(listener);
        }
    }

    void addDrawRecordedListener(DrawRecordedListener* listener) {
        if (listener != nullptr) {
            m_drawListeners.push_back(listener);
        }
    }

    // At most one, because two things editing the same buffer would each be
    // overwriting the other without either being able to report it.
    void setAssemblyFilter(AssemblyFilter* filter) {
        m_assemblyFilter = filter;
    }

    // At most one, for the same reason.
    void setVertexFilter(VertexFilter* filter) {
        m_vertexFilter = filter;
    }

    // The runtime's draws the renderer would take vertex bytes for, and those
    // the vertex filter gave some.
    uint64_t runtimeDrawsReplaceable() const {
        return m_runtimeDrawsReplaceable;
    }

    uint64_t runtimeDrawsReplaced() const {
        return m_runtimeDrawsReplaced;
    }

    uint64_t presentsSeen() const {
        return m_presentsSeen;
    }

    // The last frame that ended. Empty until one has. Held separately from the
    // frame being filled so a replay never reads a half-recorded frame.
    const FrameRecording& lastCompleteFrame() const {
        return m_completed;
    }

    // The frame published before that one: the other end of every blend.
    const FrameRecording& previousFrame() const {
        return m_previous;
    }

    // Denominators. A run where these stay at zero reached no draws, which is
    // a different failure from a run whose frames were all refused.
    uint64_t framesObserved() const {
        return m_framesObserved;
    }

    uint64_t framesRefusedIncomplete() const {
        return m_framesRefusedIncomplete;
    }

    uint64_t displayListsSeen() const {
        return m_displayListsSeen;
    }

    uint64_t uniformAssembliesSeen() const {
        return m_uniformAssembliesSeen;
    }

    // What came back out of the runtime's own submissions rather than the
    // guest's frame. Recording these would put a replay of frame N into the
    // recording of frame N+1, so they are counted and set aside.
    uint64_t displayListsFromRuntime() const {
        return m_displayListsFromRuntime;
    }

    uint64_t uniformAssembliesFromRuntime() const {
        return m_uniformAssembliesFromRuntime;
    }

    // How far a submitted buffer got: the packets the command processor walked
    // in it and the draws it issued from them. Zero packets means the buffer
    // was never read; packets without draws means it carried state and no
    // geometry; draws without a uniform assembly means the renderer dropped
    // them before a shader ran.
    // Buffers referenced from inside a recorded one. They are not recorded,
    // because walking the buffer that references them reaches them; counting
    // them is what shows the frame is more than the lists held.
    uint64_t nestedListsSeen() const {
        return m_nestedListsSeen;
    }

    // How much of a frame a recording can hold: draws the title issued from a
    // command buffer, against draws it issued straight from the ring, which
    // no recording of buffers can reach.
    uint64_t guestDrawsFromCommandBuffers() const {
        return m_guestDrawsFromCommandBuffers;
    }

    uint64_t guestDrawsFromRing() const {
        return m_guestDrawsFromRing;
    }

    // The title's draws as the renderer issued them, and those whose vertex
    // shader reads no uniforms: geometry placed by vertex data alone, which
    // no uniform blend moves. They are the draws interpolation cannot reach.
    uint64_t guestDrawsPrepared() const {
        return m_guestDrawsPrepared;
    }

    uint64_t guestDrawsWithoutVertexUniforms() const {
        return m_guestDrawsWithoutVertexUniforms;
    }

    // Whether the title's draws read vertex bytes it rewrote, by vertex
    // shader: those without uniforms always, the rest over a census.
    VertexChanges& vertexChanges() {
        return m_vertexChanges;
    }

    const VertexChanges& vertexChanges() const {
        return m_vertexChanges;
    }

    uint64_t runtimeSubmissions() const {
        return m_runtimeSubmissions;
    }

    uint64_t runtimePacketsProcessed() const {
        return m_runtimePacketsProcessed;
    }

    uint64_t runtimeDrawsIssued() const {
        return m_runtimeDrawsIssued;
    }

    // Packets the runtime's submissions were not allowed to execute, by class.
    // A replay that needed one of them to draw correctly shows up here rather
    // than as an image that is quietly wrong.
    uint64_t runtimeWithheld(LatteFrameHooks::WithheldEffect effect) const {
        return m_runtimeWithheld[static_cast<size_t>(effect)];
    }

  private:
    std::vector<FrameEndListener*> m_listeners;
    std::vector<AssemblyRecordedListener*> m_assemblyListeners;
    std::vector<FrameShownListener*> m_shownListeners;
    std::vector<PresentListener*> m_presentListeners;
    std::vector<DisplayedListener*> m_displayedListeners;
    std::vector<ScanOutListener*> m_scanOutListeners;
    std::vector<DrawRecordedListener*> m_drawListeners;
    FrameRecording m_inFlight;
    FrameRecording m_completed;
    RecordedUniformAssembly m_assemblyScratch;
    FrameRecording m_previous;
    std::array<uint64_t, LatteFrameHooks::kWithheldEffectCount> m_runtimeWithheld{};
    uint64_t m_framesObserved{0};
    uint64_t m_framesRefusedIncomplete{0};
    uint64_t m_displayListsSeen{0};
    uint64_t m_uniformAssembliesSeen{0};
    uint64_t m_presentsSeen{0};
    uint64_t m_displayListsFromRuntime{0};
    uint64_t m_uniformAssembliesFromRuntime{0};
    uint64_t m_nestedListsSeen{0};
    uint64_t m_guestDrawsFromCommandBuffers{0};
    uint64_t m_guestDrawsFromRing{0};
    uint64_t m_guestDrawsPrepared{0};
    uint64_t m_guestDrawsWithoutVertexUniforms{0};
    VertexChanges m_vertexChanges;
    uint64_t m_runtimeSubmissions{0};
    uint64_t m_runtimePacketsProcessed{0};
    uint64_t m_runtimeDrawsIssued{0};
    AssemblyFilter* m_assemblyFilter{nullptr};
    VertexFilter* m_vertexFilter{nullptr};
    uint64_t m_runtimeDrawsReplaceable{0};
    uint64_t m_runtimeDrawsReplaced{0};
};

} // namespace wiiuport::frame
