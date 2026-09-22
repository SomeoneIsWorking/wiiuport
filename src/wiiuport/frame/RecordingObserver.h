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
};

} // namespace wiiuport::frame
