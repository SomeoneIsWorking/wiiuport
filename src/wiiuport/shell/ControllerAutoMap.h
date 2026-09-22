#pragma once

#include "wiiuport/control/ControllerStatus.h"
#include <cstdint>
#include <memory>

#include <span>
#include <string>

union SDL_Event;
class ControllerBase;

namespace wiiuport::shell {

// One emulated button and the physical control that drives it.
struct ButtonBinding {
    uint64_t emulated;
    uint64_t physical;
};

// Attaches whatever gamepad is plugged in to the emulated GamePad, and lets
// the player swap it mid-game.
//
// It exists because the product has no settings screen to map a pad in: a
// first-party host that cannot be played with the controller already in the
// player's hands is not a product. The layout is by position, not by letter,
// so the button under the thumb is the one the Wii U had there.
class ControllerAutoMap final : public control::ControllerStatusSource {
  public:
    // The default layout, and the only place it is written down.
    static std::span<const ButtonBinding> layout();

    // Attaches the first gamepad already present, if any. Safe to call with
    // none: it reports zero attached rather than failing.
    void start();

    // Takes one host event. Device arrivals and departures are the only ones
    // it acts on; everything else is ignored without comment.
    void handleEvent(const SDL_Event& event);

    // What is attached now, empty when nothing is.
    const std::string& attachedDevice() const override {
        return m_attachedDevice;
    }

    size_t devicesAttached() const override {
        return m_devicesAttached;
    }

    size_t devicesLost() const override {
        return m_devicesLost;
    }

    size_t bindingCount() const override {
        return layout().size();
    }

  private:
    // Re-examines what is plugged in and attaches or detaches accordingly.
    // One place, so an arrival and a departure cannot disagree.
    void reconcile();
    bool attach(const std::shared_ptr<ControllerBase>& controller);

    std::string m_attachedDevice;
    size_t m_devicesAttached{0};
    size_t m_devicesLost{0};
};

} // namespace wiiuport::shell
