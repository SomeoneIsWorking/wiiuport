#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace wiiuport::interp {

// One of the title's own objects, as its code keeps it: its guest address and
// how many ticks it has lived. An object reborn at the same address -- a pool
// giving its slot to a new one -- starts younger than the one it replaced.
struct GuestObject {
    uint32_t address;
    float age;

    bool operator==(const GuestObject&) const = default;
};

// Names the title's object a draw drew, where the title's code was observed
// drawing it: what a draw's vertices cannot say for themselves.
class DrawObjects {
  public:
    virtual ~DrawObjects() = default;

    // The object whose vertices the draw read from `source` -- the guest's
    // bytes, as the renderer names them -- and whose copy is `bytes`, or none
    // when no object was seen drawn there or the bytes are not its vertices.
    // Safe from any thread.
    virtual std::optional<GuestObject> objectDrawn(const void* source,
                                                   std::span<const std::byte> bytes) const = 0;
};

} // namespace wiiuport::interp
