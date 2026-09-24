#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wiiuport::frame {

// The unit a change to guest memory is reported in.
inline constexpr uint32_t kGuestPageSize = 4096;

// Guest memory copied once and compared later, page by page, byte for byte.
//
// Exact rather than hashed, so a page reported unchanged is one whose every
// byte is. The copy is as large as the memory it holds -- over a gigabyte for
// a title -- so a diagnostic takes one when asked and releases it after.
class GuestMemorySnapshot {
  public:
    using Region = LatteFrameHooks::GuestMemoryRegion;

    void take(std::span<const Region> regions);

    // The guest address of every page whose bytes differ from the copy, in
    // ascending order. `regions` must be laid out as the ones taken: memory
    // mapped or unmapped in between is not a change this can measure, and
    // throws rather than comparing unrelated bytes.
    std::vector<uint32_t> changedPages(std::span<const Region> regions) const;

    size_t bytesHeld() const {
        return m_bytes.size();
    }

    void release();

  private:
    struct Span {
        uint32_t guestAddress;
        uint32_t size;
    };

    std::vector<uint8_t> m_bytes;
    std::vector<Span> m_layout;
};

} // namespace wiiuport::frame
