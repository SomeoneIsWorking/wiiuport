#include "check.h"
#include "suites.h"
#include "wiiuport/interp/SlotPool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

using wiiuport::interp::SlotPool;

namespace {

constexpr size_t kWorkers = 3;
constexpr size_t kSlots = 64;

void aSlotWaitedForIsDoneWhileAnEarlierOneIsNot() {
    std::atomic<bool> released{false};
    std::vector<uint64_t> written(kSlots, 0);
    SlotPool pool(kWorkers, [&](size_t slot, size_t /*worker*/) {
        if (slot == 0) {
            released.wait(false);
        }
        written[slot] = (slot * 2) + 1;
    });
    pool.start(kSlots);
    pool.waitFor(kSlots - 1);
    check::isTrue(pool.done(kSlots - 1), "the slot waited for is done");
    check::equal(written[kSlots - 1], uint64_t{(2 * (kSlots - 1)) + 1},
                 "what the slot's work wrote is seen once it is done");
    check::isTrue(!pool.done(0), "a slot still being worked on is not done");
    released = true;
    released.notify_all();
    pool.waitAll();
    check::isTrue(pool.done(0), "every slot is done once the run is");
    bool all = true;
    for (size_t slot = 0; slot < kSlots; ++slot) {
        all = all && written[slot] == (slot * 2) + 1;
    }
    check::isTrue(all, "every slot's work is seen once the run is done");
}

void eachRunDoesEachOfItsSlotsOnceOnAWorkerOfThePool() {
    std::vector<std::atomic<uint32_t>> worked(2 * kSlots);
    std::atomic<bool> outsideWorkers{false};
    SlotPool pool(kWorkers, [&](size_t slot, size_t worker) {
        worked[slot].fetch_add(1);
        if (worker >= kWorkers) {
            outsideWorkers = true;
        }
    });
    pool.start(2 * kSlots);
    pool.waitAll();
    pool.start(kSlots);
    pool.waitAll();
    pool.start(0);
    pool.waitAll();
    bool first = true;
    bool rest = true;
    for (size_t slot = 0; slot < kSlots; ++slot) {
        first = first && worked[slot].load() == 2;
        rest = rest && worked[kSlots + slot].load() == 1;
    }
    check::isTrue(first, "a slot of both runs was done once in each");
    check::isTrue(rest, "a slot of only the first run was done once");
    check::isTrue(!outsideWorkers.load(), "every slot was given a worker of the pool");
    check::isTrue(pool.busy().count() > 0, "the workers' time is counted");
}

} // namespace

namespace wiiuport::tests {

void runSlotPoolTests() {
    aSlotWaitedForIsDoneWhileAnEarlierOneIsNot();
    eachRunDoesEachOfItsSlotsOnceOnAWorkerOfThePool();
}

} // namespace wiiuport::tests
