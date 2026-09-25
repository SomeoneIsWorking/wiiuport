#include "wiiuport/guest/BufferWriters.h"

#include <algorithm>

namespace wiiuport::guest {

void BufferWriters::record(const void* source, const interp::GuestObject& object,
                           const Leading& leading) {
    m_calls.fetch_add(1);
    std::lock_guard lock(m_mutex);
    m_bySource.insert_or_assign(source, Written{object, leading});
}

std::optional<interp::GuestObject>
BufferWriters::objectDrawn(const void* source, std::span<const std::byte> bytes) const {
    std::lock_guard lock(m_mutex);
    auto found = m_bySource.find(source);
    if (found == m_bySource.end()) {
        return std::nullopt;
    }
    const Written& written = found->second;
    if (bytes.size() < kLeadingBytes ||
        !std::ranges::equal(bytes.first(kLeadingBytes), written.leading)) {
        m_rewritten.fetch_add(1);
        return std::nullopt;
    }
    m_identified.fetch_add(1);
    return written.object;
}

} // namespace wiiuport::guest
