#include "wiiuport/frame/GuestMemorySnapshot.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace wiiuport::frame {

void GuestMemorySnapshot::take(std::span<const Region> regions) {
    size_t total = 0;
    m_layout.clear();
    for (const Region& region : regions) {
        total += region.size;
        m_layout.push_back({.guestAddress = region.guestAddress, .size = region.size});
    }
    m_bytes.resize(total);
    size_t offset = 0;
    for (const Region& region : regions) {
        std::memcpy(m_bytes.data() + offset, region.bytes, region.size);
        offset += region.size;
    }
}

std::vector<uint32_t> GuestMemorySnapshot::changedPages(std::span<const Region> regions) const {
    if (regions.size() != m_layout.size()) {
        throw std::logic_error("guest memory was mapped or unmapped since the snapshot");
    }
    std::vector<uint32_t> changed;
    size_t offset = 0;
    for (size_t index = 0; index < regions.size(); ++index) {
        const Region& region = regions[index];
        if (region.guestAddress != m_layout[index].guestAddress ||
            region.size != m_layout[index].size) {
            throw std::logic_error("guest memory was mapped or unmapped since the snapshot");
        }
        for (uint32_t page = 0; page < region.size; page += kGuestPageSize) {
            uint32_t length = std::min(kGuestPageSize, region.size - page);
            if (std::memcmp(region.bytes + page, m_bytes.data() + offset + page, length) != 0) {
                changed.push_back(region.guestAddress + page);
            }
        }
        offset += region.size;
    }
    return changed;
}

void GuestMemorySnapshot::release() {
    m_bytes = {};
    m_layout = {};
}

} // namespace wiiuport::frame
