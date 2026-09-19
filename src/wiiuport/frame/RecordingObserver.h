#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/FrameRecording.h"

#include <cstdint>

namespace wiiuport::frame {

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
    void OnFrameEnd() override;

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
    FrameRecording m_inFlight;
    FrameRecording m_completed;
    uint64_t m_framesObserved{0};
    uint64_t m_framesRefusedIncomplete{0};
    uint64_t m_displayListsSeen{0};
    uint64_t m_uniformAssembliesSeen{0};
};

} // namespace wiiuport::frame
