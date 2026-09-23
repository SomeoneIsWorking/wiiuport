#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

namespace wiiuport::frame {

// Keeps what the runtime draws over, so the guest's frame can be put back by
// copying it instead of drawing it a second time.
//
// Opened before the runtime draws a frame of its own over the guest's and
// restored before the guest's next packet; the fork copies each texture
// subresource aside at its first write in between, and back at the restore.
// What a copy cannot undo is counted in the restore, and the caller decides
// what to do about it.
class GuestStateGuard {
  public:
    using Open = void (*)();
    using Restore = LatteFrameHooks::GuestStateRestore (*)();

    // Injected so a test drives the owner without a renderer.
    GuestStateGuard(Open open, Restore restore);

    // Throws std::logic_error when already open: a second open would drop
    // the copies of the first, and the frame they belong to with them.
    void open();
    // Throws std::logic_error when not open.
    LatteFrameHooks::GuestStateRestore restore();

    bool isOpen() const {
        return m_open;
    }

  private:
    Open m_openGuard;
    Restore m_restoreGuard;
    bool m_open{false};
};

} // namespace wiiuport::frame
