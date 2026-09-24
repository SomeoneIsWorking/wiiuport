#include "check.h"
#include "suites.h"
#include "wiiuport/frame/FrameGate.h"
#include "wiiuport/frame/FrameRecording.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

using wiiuport::frame::FrameGate;
using wiiuport::frame::FrameRecording;

namespace {

// The rendering thread as the gate meets it: frames ending one after
// another until told to stop.
class FrameThread {
  public:
    explicit FrameThread(FrameGate& gate)
        : m_thread([this, &gate] {
              FrameRecording recording;
              while (!m_stop.load()) {
                  gate.onFrameShown(recording);
                  m_ended.fetch_add(1);
                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
              }
          }) {
    }

    ~FrameThread() {
        m_stop.store(true);
        m_thread.join();
    }

    FrameThread(const FrameThread&) = delete;
    FrameThread& operator=(const FrameThread&) = delete;

    uint64_t ended() const {
        return m_ended.load();
    }

  private:
    std::atomic<bool> m_stop{false};
    std::atomic<uint64_t> m_ended{0};
    std::thread m_thread;
};

void aRunningGateLetsEveryFrameThrough() {
    FrameGate gate;
    FrameRecording recording;
    for (int frame = 0; frame < 5; ++frame) {
        gate.onFrameShown(recording);
    }
    check::equal(gate.status().framesHeld, uint64_t{0}, "no frame is held");
}

void aPausedGateHoldsAndStepsExactlyAsAsked() {
    FrameGate gate;
    gate.pause();
    FrameThread frames(gate);
    check::isTrue(gate.awaitHeld(std::chrono::seconds(5)), "the paused gate holds");
    uint64_t heldAt = frames.ended();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check::equal(frames.ended(), heldAt, "and no frame ends while it holds");

    gate.step(3);
    check::isTrue(gate.awaitHeld(std::chrono::seconds(5)), "a step ends by holding again");
    check::equal(frames.ended(), heldAt + 3, "after exactly the frames asked for");
    check::equal(gate.status().framesStepped, uint64_t{3}, "all counted as stepped");

    gate.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check::isTrue(frames.ended() > heldAt + 4, "resumed, frames run on");
    check::isTrue(!gate.status().holding, "and nothing is held");
}

void aReleasedGateNeverHoldsAgain() {
    FrameGate gate;
    gate.pause();
    FrameThread frames(gate);
    check::isTrue(gate.awaitHeld(std::chrono::seconds(5)), "held before shutdown");
    gate.release();
    gate.pause();
    uint64_t released = frames.ended();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check::isTrue(frames.ended() > released, "released at shutdown, a pause cannot hold it");
    check::isTrue(!gate.awaitHeld(std::chrono::milliseconds(20)), "and waiting for it fails");
}

} // namespace

namespace wiiuport::tests {

void runGateTests() {
    aRunningGateLetsEveryFrameThrough();
    aPausedGateHoldsAndStepsExactlyAsAsked();
    aReleasedGateNeverHoldsAgain();
}

} // namespace wiiuport::tests
