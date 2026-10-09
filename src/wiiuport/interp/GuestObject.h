#pragma once

#include <cstdint>
#include <optional>

namespace wiiuport::interp {

// One of the title's own objects, as its code keeps it: its guest address and
// how long it has lived. An object reborn at the same address -- a pool
// giving its slot to a new one -- starts younger than the one it replaced.
// An object the title never renews in place -- a 3D line, living as long as
// what holds it -- has no age.
struct GuestObject {
    uint32_t address;
    std::optional<float> age;

    bool operator==(const GuestObject&) const = default;

    // Whether this, written before, is `later` still: the same address, and
    // either both unaged or this no older -- a tick's two paints write one age.
    bool continuesAs(const GuestObject& later) const {
        if (address != later.address || age.has_value() != later.age.has_value()) {
            return false;
        }
        return !age.has_value() || *age <= *later.age;
    }
};

} // namespace wiiuport::interp
