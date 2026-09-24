#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/interp/ObjectCensus.h"
#include "wiiuport/interp/ObjectPlanner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
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

    // Off, draws into the light's map are drawn as the title drew them. A
    // maintainer's discriminator, not a setting. Safe from any thread.
    void setMapBlending(bool blending) {
        m_mapBlending.store(blending);
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

    bool isPlanning() const {
        return m_planning.load();
    }

    // Times armed: each is one replay.
    uint64_t armings() const {
        return m_armings;
    }

    // The entry of the latest frame the replay's next assembly is: how many
    // of its assemblies the replay has drawn.
    size_t replayCursor() const {
        return m_replayCursor;
    }

    // Writes one replayed draw's blend, if it has one.
    bool apply(const LatteFrameHooks::UniformAssembly& assembly);

    // Objects of every armed frame, by what they came to. Their sum is the
    // objects armed; none is dropped without a reason.
    uint64_t objects(Outcome outcome) const {
        return m_outcomes[static_cast<size_t>(outcome)];
    }

    const ObjectPlanner::Outcomes& outcomes() const {
        return m_outcomes;
    }

    const ObjectPlanner& planner() const {
        return m_planner;
    }

    // Draws the objects of the shaders with these base hashes as the title
    // drew them from the next arming on. A maintainer's discriminator, not a
    // setting. Safe from any thread.
    void exclude(std::vector<uint64_t> shaderBaseHashes) {
        std::sort(shaderBaseHashes.begin(), shaderBaseHashes.end());
        std::lock_guard lock(m_excludedMutex);
        m_excluded = std::move(shaderBaseHashes);
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

    // Frames the guest ended while planning, and how long their ends spent
    // on it: waiting for the planner to catch up, then indexing the frame for
    // the searches of the frames after it. A cost the title's own frame pays.
    uint64_t framesEnded() const {
        return m_framesEnded.load();
    }

    std::chrono::nanoseconds frameEndPlanning() const {
        return std::chrono::nanoseconds{m_frameEndPlanningNanoseconds.load()};
    }

    // How long the planning thread spent planning, frame ends apart: against
    // the frames' ends, whether a long wait is planning's own work or the
    // thread waiting for a core.
    std::chrono::nanoseconds planningBusy() const {
        return std::chrono::nanoseconds{m_planningBusyNanoseconds.load()};
    }

    // A minute of the title's frames: a census long enough to be steady.
    static constexpr size_t kMaxCensusFrames = 1800;

    // Asks for a census of the next `frames` planned frames, replacing any
    // taken or being taken. Safe from any thread.
    void requestCensus(uint32_t frames) {
        m_censusRequested.store(frames);
    }

    // The census last completed; none until one was requested and its frames
    // were planned. Safe from any thread.
    std::optional<ObjectCensus> census() const;

  private:
    // Draws handed over before the planning thread is woken for them: waking
    // it for every draw would cost more than planning one.
    static constexpr size_t kWakeBatch = 32;

    void planHandedOver(const std::stop_token& stop);
    // Returns once every draw handed over has been planned.
    void waitUntilPlanned();
    // Adds the frame just planned to a requested census.
    void tallyCensus();

    ObjectPlanner m_planner;
    std::atomic<bool> m_planning{false};
    std::atomic<bool> m_mapBlending{true};

    // Handed over and not yet taken, and the batch the planning thread is
    // working through. They swap, so both keep their capacity.
    std::mutex m_mutex;
    std::condition_variable_any m_handedOver;
    std::condition_variable m_caughtUp;
    std::vector<frame::RecordedUniformAssembly> m_pending;
    size_t m_pendingCount{0};
    std::vector<frame::RecordedUniformAssembly> m_taken;
    bool m_planningBatch{false};

    std::mutex m_excludedMutex;
    std::vector<uint64_t> m_excluded;
    // m_excluded as it was at the arming, read by the replay without a lock.
    std::vector<uint64_t> m_armedExcluded;

    size_t m_replayCursor{0};
    uint64_t m_armings{0};
    bool m_armed{false};
    ObjectPlanner::Outcomes m_outcomes{};
    uint64_t m_drawsWritten{0};
    uint64_t m_replaysDiverged{0};
    std::atomic<uint64_t> m_framesEnded{0};
    std::atomic<int64_t> m_frameEndPlanningNanoseconds{0};
    std::atomic<int64_t> m_planningBusyNanoseconds{0};

    // Frames asked for and not yet taken up, then the tally taking them.
    std::atomic<uint32_t> m_censusRequested{0};
    uint32_t m_censusWanted{0};
    CensusTally m_tally;
    mutable std::mutex m_censusMutex;
    std::optional<ObjectCensus> m_census;

    // Last, so it starts after and stops before everything it uses.
    std::jthread m_thread;
};

} // namespace wiiuport::interp
