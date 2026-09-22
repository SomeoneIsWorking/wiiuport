#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/FrameRecording.h"

#include <cstdint>
#include <vector>

namespace wiiuport::frame {

// Notified once a frame is complete and published, which is the only moment
// anything may act on a whole frame. Kept as a narrow interface so the
// recorder does not acquire an opinion about what happens next.
class FrameEndListener {
  public:
    virtual ~FrameEndListener() = default;
    virtual void onFrameRecorded(const FrameRecording& recording) = 0;
};

// Notified for every present the title makes. Separate from FrameEndListener
// because a present and a finished recording are different moments: the
// arguments arrive with the copy packet, before the swap.
class PresentListener {
  public:
    virtual ~PresentListener() = default;
    virtual void onPresentObserved(const LatteFrameHooks::PresentArguments& present) = 0;
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
    void OnFrameEnd() override;
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

    void addPresentListener(PresentListener* listener) {
        if (listener != nullptr) {
            m_presentListeners.push_back(listener);
        }
    }

    // At most one, because two things editing the same buffer would each be
    // overwriting the other without either being able to report it.
    void setAssemblyFilter(AssemblyFilter* filter) {
        m_assemblyFilter = filter;
    }

    uint64_t presentsSeen() const {
        return m_presentsSeen;
    }

    // The last frame that ended. Empty until one has. Held separately from the
    // frame being filled so a replay never reads a half-recorded frame.
    const FrameRecording& lastCompleteFrame() const {
        return m_completed;
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

    uint64_t runtimeSubmissions() const {
        return m_runtimeSubmissions;
    }

    uint64_t runtimePacketsProcessed() const {
        return m_runtimePacketsProcessed;
    }

    uint64_t runtimeDrawsIssued() const {
        return m_runtimeDrawsIssued;
    }

  private:
    std::vector<FrameEndListener*> m_listeners;
    std::vector<PresentListener*> m_presentListeners;
    FrameRecording m_inFlight;
    FrameRecording m_completed;
    uint64_t m_framesObserved{0};
    uint64_t m_framesRefusedIncomplete{0};
    uint64_t m_displayListsSeen{0};
    uint64_t m_uniformAssembliesSeen{0};
    uint64_t m_presentsSeen{0};
    uint64_t m_displayListsFromRuntime{0};
    uint64_t m_uniformAssembliesFromRuntime{0};
    uint64_t m_nestedListsSeen{0};
    uint64_t m_runtimeSubmissions{0};
    uint64_t m_runtimePacketsProcessed{0};
    uint64_t m_runtimeDrawsIssued{0};
    AssemblyFilter* m_assemblyFilter{nullptr};
};

} // namespace wiiuport::frame
