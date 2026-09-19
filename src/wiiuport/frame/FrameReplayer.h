#pragma once

#include "wiiuport/frame/FrameRecording.h"

#include <cstdint>

namespace wiiuport::frame {

// Re-feeds a recorded frame's display lists to the command processor.
//
// Armed one frame at a time and disarmed by firing, because the first
// question a replay has to answer is whether it can be done at all without
// disturbing the run. A replay that repeated every frame would make a crash
// impossible to attribute to one submission.
//
// The submission itself is injected, so this is testable without an emulator
// and the test drives the same code the product does.
class FrameReplayer {
  public:
    // Returns false when the buffer was refused, which the replayer counts
    // rather than treating as success.
    using Submit = bool (*)(const void* data, uint32_t sizeInBytes);

    explicit FrameReplayer(Submit submit);

    void armOnce() {
        m_armed = true;
    }

    bool isArmed() const {
        return m_armed;
    }

    // Does nothing unless armed. Returns the number of lists submitted.
    uint64_t replayIfArmed(const FrameRecording& recording);

    uint64_t replaysRun() const {
        return m_replaysRun;
    }

    uint64_t listsSubmitted() const {
        return m_listsSubmitted;
    }

    uint64_t listsRefused() const {
        return m_listsRefused;
    }

  private:
    Submit m_submit;
    bool m_armed{false};
    uint64_t m_replaysRun{0};
    uint64_t m_listsSubmitted{0};
    uint64_t m_listsRefused{0};
};

} // namespace wiiuport::frame
