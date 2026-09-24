#include "wiiuport/interp/SlotPool.h"

#include <utility>

namespace wiiuport::interp {

SlotPool::SlotPool(size_t workers, Work work) : m_work(std::move(work)) {
    m_workers.reserve(workers);
    for (size_t worker = 0; worker < workers; ++worker) {
        m_workers.emplace_back([this, worker](const std::stop_token& stop) {
            serve(stop, worker);
        });
    }
}

SlotPool::~SlotPool() {
    for (std::jthread& worker : m_workers) {
        worker.request_stop();
    }
    m_started.notify_all();
}

void SlotPool::start(size_t slots) {
    std::unique_lock lock(m_mutex);
    m_idle.wait(lock, [this] {
        return m_inRun == 0;
    });
    if (slots > m_capacity) {
        m_done = std::make_unique<std::atomic<uint8_t>[]>(slots);
        m_capacity = slots;
    }
    for (size_t slot = 0; slot < slots; ++slot) {
        m_done[slot].store(0, std::memory_order_relaxed);
    }
    m_finished.store(0, std::memory_order_relaxed);
    m_next.store(0, std::memory_order_relaxed);
    m_slots = slots;
    ++m_run;
    lock.unlock();
    m_started.notify_all();
}

bool SlotPool::done(size_t slot) const {
    return m_done[slot].load(std::memory_order_acquire) != 0;
}

void SlotPool::waitFor(size_t slot) const {
    m_done[slot].wait(0, std::memory_order_acquire);
}

void SlotPool::waitAll() const {
    size_t finished = m_finished.load(std::memory_order_acquire);
    while (finished < m_slots) {
        m_finished.wait(finished, std::memory_order_acquire);
        finished = m_finished.load(std::memory_order_acquire);
    }
}

void SlotPool::serve(const std::stop_token& stop, size_t worker) {
    uint64_t served = 0;
    std::unique_lock lock(m_mutex);
    while (m_started.wait(lock, stop, [&] {
        return m_run != served;
    })) {
        served = m_run;
        size_t slots = m_slots;
        ++m_inRun;
        lock.unlock();
        auto started = std::chrono::steady_clock::now();
        for (size_t slot = m_next.fetch_add(1); slot < slots; slot = m_next.fetch_add(1)) {
            m_work(slot, worker);
            m_done[slot].store(1, std::memory_order_release);
            m_done[slot].notify_all();
            m_finished.fetch_add(1, std::memory_order_acq_rel);
            m_finished.notify_all();
        }
        m_busyNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        lock.lock();
        if (--m_inRun == 0) {
            m_idle.notify_all();
        }
    }
}

} // namespace wiiuport::interp
