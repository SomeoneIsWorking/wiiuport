#pragma once

#include <cstddef>
#include <string>

namespace wiiuport::control {

// What the first-run setup screen is doing, asked from outside the process.
//
// The screen blocks the product until a player answers it, so without this an
// agent's run would sit at a window it cannot see and cannot drive, and a
// failure to open would look exactly like a player taking their time. The
// host registers the screen while it is up and unregisters it afterwards.
class SetupStatusSource {
  public:
    virtual ~SetupStatusSource() = default;

    // Whether the screen is on the display right now.
    virtual bool setupShown() const = 0;

    // What the screen is waiting for: "collecting", "ready", "rejected" or
    // "accepted".
    virtual std::string setupState() const = 0;

    // How many selections the player has handed it, so "nothing chosen" can
    // be told from "chose something the product refused".
    virtual size_t setupSelectionsOffered() const = 0;
};

} // namespace wiiuport::control
