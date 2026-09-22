#pragma once

#include <cstddef>
#include <string>

namespace wiiuport::control {

// What the host can say about the physical controller it has attached.
//
// The control channel is in the library and the thing that attaches pads is
// in the host, so the host registers itself through this interface rather
// than the library reaching upwards into it.
class ControllerStatusSource {
  public:
    virtual ~ControllerStatusSource() = default;

    // The attached device's name, empty when none is.
    virtual const std::string& attachedDevice() const = 0;

    // Denominators, so "no controller" can be told from "never looked".
    virtual size_t devicesAttached() const = 0;
    virtual size_t devicesLost() const = 0;
    virtual size_t bindingCount() const = 0;
};

} // namespace wiiuport::control
