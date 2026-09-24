#include "wiiuport/interp/ObjectBlend.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace wiiuport::interp {

ObjectBlend::ObjectBlend(float t)
    : m_planner(t), m_thread([this](const std::stop_token& stop) {
          planHandedOver(stop);
      }) {
}

void ObjectBlend::onAssemblyRecorded(const frame::RecordedUniformAssembly& assembly) {
    if (!m_planning.load()) {
        return;
    }
    std::lock_guard lock(m_mutex);
    // Assigned over a slot from an earlier frame, so its vectors keep their
    // capacity and handing a draw over allocates nothing.
    if (m_pendingCount == m_pending.size()) {
        m_pending.push_back(assembly);
    } else {
        m_pending[m_pendingCount] = assembly;
    }
    ++m_pendingCount;
    if (m_pendingCount >= kWakeBatch && !m_planningBatch) {
        m_handedOver.notify_one();
    }
}

void ObjectBlend::onFrameRecorded(const frame::FrameRecording& /*recording*/) {
    auto started = std::chrono::steady_clock::now();
    waitUntilPlanned();
    if (!m_planning.load()) {
        m_planner.forget();
        return;
    }
    m_planner.endFrame();
    m_frameEndPlanningNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count();
    ++m_framesEnded;
    tallyCensus();
}

void ObjectBlend::tallyCensus() {
    if (uint32_t requested = m_censusRequested.exchange(0); requested != 0) {
        m_censusWanted = requested;
        m_tally = {};
    }
    if (m_censusWanted == 0 || !m_planner.ready()) {
        return;
    }
    m_tally.add(m_planner);
    if (m_tally.frames() < m_censusWanted) {
        return;
    }
    ObjectCensus census = m_tally.census();
    m_censusWanted = 0;
    std::lock_guard lock(m_censusMutex);
    m_census = std::move(census);
}

std::optional<ObjectCensus> ObjectBlend::census() const {
    std::lock_guard lock(m_censusMutex);
    return m_census;
}

void ObjectBlend::waitUntilPlanned() {
    std::unique_lock lock(m_mutex);
    m_handedOver.notify_one();
    m_caughtUp.wait(lock, [this] {
        return m_pendingCount == 0 && !m_planningBatch;
    });
}

void ObjectBlend::planHandedOver(const std::stop_token& stop) {
    std::unique_lock lock(m_mutex);
    while (m_handedOver.wait(lock, stop, [this] {
        return m_pendingCount > 0;
    })) {
        std::swap(m_pending, m_taken);
        size_t count = std::exchange(m_pendingCount, 0);
        m_planningBatch = true;
        lock.unlock();
        auto started = std::chrono::steady_clock::now();
        m_planner.setMapBlending(m_mapBlending.load());
        m_planner.setPixelBlending(m_pixelBlending.load());
        for (size_t index = 0; index < count; ++index) {
            m_planner.add(m_taken[index]);
        }
        m_planningBusyNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count();
        lock.lock();
        m_planningBatch = false;
        m_caughtUp.notify_all();
    }
}

bool ObjectBlend::armOnce() {
    m_armed = false;
    if (!m_planner.ready()) {
        return false;
    }
    const ObjectPlanner::Outcomes& planned = m_planner.latestOutcomes();
    for (size_t outcome = 0; outcome < planned.size(); ++outcome) {
        m_outcomes[outcome] += planned[outcome];
    }
    {
        std::lock_guard lock(m_excludedMutex);
        m_armedExcluded = m_excluded;
    }
    m_replayCursor = 0;
    ++m_armings;
    m_armed = true;
    return true;
}

bool ObjectBlend::apply(const LatteFrameHooks::UniformAssembly& assembly) {
    if (!m_armed) {
        return false;
    }
    const KeyedFrame& latest = m_planner.latest();
    AssemblyKey drawn(
        ShaderKey{assembly.shaderBaseHash, assembly.shaderAuxHash, assembly.stageIndex},
        frame::sourceWordsOf(assembly));
    if (m_replayCursor >= latest.size() || !latest.key(m_replayCursor).describes(drawn)) {
        ++m_replaysDiverged;
        m_armed = false;
        return false;
    }
    size_t entry = m_replayCursor++;
    std::span<const float> blend = m_planner.blendOf(entry);
    if (blend.empty() || std::binary_search(m_armedExcluded.begin(), m_armedExcluded.end(),
                                            assembly.shaderBaseHash)) {
        return false;
    }
    if (blend.size_bytes() != assembly.sizeInBytes) {
        ++m_replaysDiverged;
        m_armed = false;
        return false;
    }
    std::memcpy(assembly.data, blend.data(), blend.size_bytes());
    ++m_drawsWritten;
    return true;
}

} // namespace wiiuport::interp
