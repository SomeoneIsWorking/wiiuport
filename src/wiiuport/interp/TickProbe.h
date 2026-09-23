#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <vector>

namespace wiiuport::interp {

// The points inside an interpolated tick where a diagnostic may act, in the
// order they come. Only a tick that goes on to draw an in-between frame
// reaches the first, and every tick that reaches it reaches the last.
class TickProbe {
  public:
    virtual ~TickProbe() = default;

    // The guest's frame is complete and nothing of the runtime's is drawn.
    // `tick` counts the title's frames, interpolated or not, so a probe can
    // tell the tick after another from one further on.
    virtual void beforeInBetween(uint64_t tick) = 0;
    // The in-between frame is drawn and is about to be presented.
    virtual void beforeInBetweenPresent() = 0;
    // The guest's frame has been put back and is about to be copied to the
    // scan buffer. A capture is taken at that copy, not at the swap after it.
    virtual void beforeGuestFrameCopied() = 0;
    // The tick is over, whether or not the guest's frame had to be copied.
    virtual void afterTick() = 0;
};

// Several probes at the same points, each told in the order given.
class TickProbes final : public TickProbe {
  public:
    TickProbes(std::initializer_list<std::reference_wrapper<TickProbe>> probes) : m_probes(probes) {
    }

    void beforeInBetween(uint64_t tick) override {
        for (TickProbe& probe : m_probes) {
            probe.beforeInBetween(tick);
        }
    }

    void beforeInBetweenPresent() override {
        for (TickProbe& probe : m_probes) {
            probe.beforeInBetweenPresent();
        }
    }

    void beforeGuestFrameCopied() override {
        for (TickProbe& probe : m_probes) {
            probe.beforeGuestFrameCopied();
        }
    }

    void afterTick() override {
        for (TickProbe& probe : m_probes) {
            probe.afterTick();
        }
    }

  private:
    std::vector<std::reference_wrapper<TickProbe>> m_probes;
};

} // namespace wiiuport::interp
