#pragma once

namespace wiiuport::control {

// How the channel asks the host to stop, as a player closing its window does.
//
// A signal cannot stand in for it: the emulated system's own handler ends the
// process on SIGINT and SIGTERM without the host's shutdown, so a run stopped
// that way never exercises the path a player takes. The host registers itself
// while a title runs and unregisters before it shuts down.
class HostStopTarget {
  public:
    virtual ~HostStopTarget() = default;

    // Asks the host to leave its event loop. Called from the channel's own
    // threads, so it only posts the request; the host acts on its own thread.
    virtual void requestStop() = 0;
};

} // namespace wiiuport::control
