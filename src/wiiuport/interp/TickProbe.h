#pragma once

namespace wiiuport::interp {

// The points inside an interpolated tick where a diagnostic may act, in the
// order they come. Only a tick that goes on to draw an in-between frame
// reaches the first, and every tick that reaches it reaches the last.
class TickProbe {
  public:
    virtual ~TickProbe() = default;

    // The guest's frame is complete and nothing of the runtime's is drawn.
    virtual void beforeInBetween() = 0;
    // The in-between frame is drawn and is about to be presented.
    virtual void beforeInBetweenPresent() = 0;
    // The guest's frame has been put back and is about to be copied to the
    // scan buffer. A capture is taken at that copy, not at the swap after it.
    virtual void beforeGuestFrameCopied() = 0;
    // The tick is over, whether or not the guest's frame had to be copied.
    virtual void afterTick() = 0;
};

} // namespace wiiuport::interp
