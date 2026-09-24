#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace wiiuport::interp {

// Runs one run's independent pieces of work -- slots -- on threads of its
// own, each slot claimed once and in order, so the first slots are done
// first; a reader waits for the one slot it needs rather than for the run.
//
// One run at a time: start() may be called only once the run before it is
// wholly done (waitAll). `work` is called with the slot and the worker, so a
// worker can keep scratch storage of its own; slots must not share what they
// write.
class SlotPool {
  public:
    using Work = std::function<void(size_t slot, size_t worker)>;

    SlotPool(size_t workers, Work work);
    ~SlotPool();

    SlotPool(const SlotPool&) = delete;
    SlotPool& operator=(const SlotPool&) = delete;

    // Begins a run of `slots` slots, once every worker has left the run
    // before; returns without waiting for any slot.
    void start(size_t slots);

    // Whether the current run's slot is done; what its work wrote is visible
    // to the caller once this is true.
    bool done(size_t slot) const;
    // Blocks until the current run's slot is done.
    void waitFor(size_t slot) const;
    // Blocks until every slot of the current run is done.
    void waitAll() const;

    // Time the workers spent working, summed over workers and runs.
    std::chrono::nanoseconds busy() const {
        return std::chrono::nanoseconds{m_busyNanoseconds.load()};
    }

  private:
    void serve(const std::stop_token& stop, size_t worker);

    Work m_work;
    size_t m_slots{0};
    size_t m_capacity{0};
    std::unique_ptr<std::atomic<uint8_t>[]> m_done;
    std::atomic<size_t> m_next{0};
    std::atomic<size_t> m_finished{0};
    std::atomic<int64_t> m_busyNanoseconds{0};

    std::mutex m_mutex;
    std::condition_variable_any m_started;
    std::condition_variable m_idle;
    uint64_t m_run{0};
    // Workers inside a run: a run's slots are reset only once none is, so a
    // worker leaving one run cannot claim a slot of the next.
    size_t m_inRun{0};

    // Last, so they start after and stop before everything they use.
    std::vector<std::jthread> m_workers;
};

} // namespace wiiuport::interp
