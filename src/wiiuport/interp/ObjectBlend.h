#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/interp/ObjectPlanner.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace wiiuport::interp {

// Draws every object of an in-between frame where ObjectPlanner says it
// stood.
//
// The planning runs on its own thread. The thread that records the guest's
// draws is the thread that renders them, and the title waits on it at every
// sync point in its frame: planning there was measured costing the title 3
// of its 30 Hz. Here that thread only hands each draw over; at the frame's
// end it waits for the planner to catch up, which it normally already has.
class ObjectBlend final : public frame::AssemblyRecordedListener, public frame::FrameEndListener {
  public:
    using Outcome = ObjectPlanner::Outcome;

    // `t` is where the in-between frame sits on N-1..N.
    explicit ObjectBlend(float t);

    // Off, nothing is planned or kept, and turning it back on needs three
    // new frames before anything is blended. Safe from any thread.
    void setPlanning(bool planning) {
        m_planning.store(planning);
    }

    void onAssemblyRecorded(const frame::RecordedUniformAssembly& assembly) override;
    void onFrameRecorded(const frame::FrameRecording& recording) override;

    // Readies the latest frame's plan for its replay. False until three
    // frames have been planned in a row.
    bool armOnce();

    void disarm() {
        m_armed = false;
    }

    bool isArmed() const {
        return m_armed;
    }

    // Writes one replayed draw's blend, if it has one.
    bool apply(const LatteFrameHooks::UniformAssembly& assembly);

    // Objects of every armed frame, by what they came to. Their sum is the
    // objects armed; none is dropped without a reason.
    uint64_t objects(Outcome outcome) const {
        return m_outcomes[static_cast<size_t>(outcome)];
    }

    const ObjectPlanner& planner() const {
        return m_planner;
    }

    // Replayed draws written, and replays that fell out of step with the
    // recording. The replay re-issues the recorded frame, so its n-th draw is
    // the recording's n-th; one that is not stops the blend for the rest of
    // that frame rather than writing one object's blend into another's draw.
    uint64_t drawsWritten() const {
        return m_drawsWritten;
    }

    uint64_t replaysDiverged() const {
        return m_replaysDiverged;
    }

  private:
    // Draws handed over before the planning thread is woken for them: waking
    // it for every draw would cost more than planning one.
    static constexpr size_t kWakeBatch = 32;

    void planHandedOver(std::stop_token stop);
    // Returns once every draw handed over has been planned.
    void waitUntilPlanned();

    ObjectPlanner m_planner;
    std::atomic<bool> m_planning{false};

    // Handed over and not yet taken, and the batch the planning thread is
    // working through. They swap, so both keep their capacity.
    std::mutex m_mutex;
    std::condition_variable_any m_handedOver;
    std::condition_variable m_caughtUp;
    std::vector<frame::RecordedUniformAssembly> m_pending;
    size_t m_pendingCount{0};
    std::vector<frame::RecordedUniformAssembly> m_taken;
    bool m_planningBatch{false};

    size_t m_replayCursor{0};
    bool m_armed{false};
    ObjectPlanner::Outcomes m_outcomes{};
    uint64_t m_drawsWritten{0};
    uint64_t m_replaysDiverged{0};

    // Last, so it starts after and stops before everything it uses.
    std::jthread m_thread;
};

} // namespace wiiuport::interp
