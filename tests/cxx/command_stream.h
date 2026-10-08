#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/title/CommandStreamIdentity.h"

#include <cstdint>

namespace wiiuport::tests {

// One command buffer that binds and draws are written into in order, as the guest writes them.
class CommandStream {
  public:
    static constexpr uintptr_t kStart = 0x10000000u;
    static constexpr uintptr_t kEnd = 0x20000000u;
    static constexpr uintptr_t kPacketBytes = 16;

    title::CommandStreamIdentity& identity() {
        return m_identity;
    }

    void bind(uint32_t object) {
        m_identity.bind(object);
        m_write += kPacketBytes;
    }

    uintptr_t packet() {
        const uintptr_t at = m_write;
        m_write += kPacketBytes;
        return at;
    }

    LatteFrameHooks::DrawPrepared at(LatteFrameHooks::DrawPrepared draw) {
        draw.packet = packet();
        return draw;
    }

  private:
    uintptr_t m_write = kStart;
    title::CommandStreamIdentity m_identity{[this] {
        return title::CommandPosition{kStart, kEnd, m_write};
    }};
};

} // namespace wiiuport::tests
