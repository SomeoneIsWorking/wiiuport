#pragma once

#include "wiiuport/frame/RecordingObserver.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace wiiuport::frame {

// Holds the title between frames, so an agent can look at one and step on.
//
// Paused, the rendering thread waits at the end of the frame the guest just
// swapped, after every other frame-shown listener has run; the title's CPU
// follows within a frame, since it waits on its swaps. Stepping lets that many
// more frames end and holds at the next. Nothing is dropped or fast-forwarded:
// every frame still runs whole, only later.
//
// Fed on the rendering thread, driven from the control channel. Released for
// good at shutdown, so a run paused by a maintainer cannot hang the product's
// exit.
class FrameGate final : public FrameShownListener {
  public:
    struct Status {
        bool paused{false};
        // Whether the rendering thread is waiting here now.
        bool holding{false};
        // Frames still to let end before holding.
        uint64_t stepsLeft{0};
        uint64_t framesHeld{0};
        uint64_t framesStepped{0};
    };

    // Holds at the next frame's end.
    void pause();
    // Lets `frames` more frames end, then holds. Pauses if running.
    void step(uint64_t frames);
    void resume();
    // Resumes and never holds again.
    void release();

    // Waits until the gate holds with no steps left: a step's frames have all
    // ended. False when `timeout` passed first, or the gate is not paused.
    bool awaitHeld(std::chrono::milliseconds timeout);

    Status status() const;

    void onFrameShown(const FrameRecording& recording) override;

  private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_released{false};
    Status m_status;
};

} // namespace wiiuport::frame
