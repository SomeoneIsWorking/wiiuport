#include "wiiuport/frame/GuestStateGuard.h"

#include <stdexcept>

namespace wiiuport::frame {

GuestStateGuard::GuestStateGuard(Open open, Restore restore)
    : m_openGuard(open), m_restoreGuard(restore) {
}

void GuestStateGuard::open() {
    if (m_open) {
        throw std::logic_error("the guest-state guard is already open");
    }
    m_openGuard();
    m_open = true;
}

LatteFrameHooks::GuestStateRestore GuestStateGuard::restore() {
    if (!m_open) {
        throw std::logic_error("the guest-state guard is not open");
    }
    m_open = false;
    return m_restoreGuard();
}

} // namespace wiiuport::frame
