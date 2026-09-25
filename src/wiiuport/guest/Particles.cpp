#include "wiiuport/guest/Particles.h"

#include <algorithm>

namespace wiiuport::guest {

void Particles::record(const void* source, const interp::GuestObject& object, const Quad& quad) {
    m_calls.fetch_add(1);
    std::lock_guard lock(m_mutex);
    m_bySource.insert_or_assign(source, Written{object, quad});
}

std::optional<interp::GuestObject> Particles::objectDrawn(const void* source,
                                                          std::span<const std::byte> bytes) const {
    std::lock_guard lock(m_mutex);
    auto found = m_bySource.find(source);
    if (found == m_bySource.end()) {
        return std::nullopt;
    }
    const Written& written = found->second;
    if (bytes.size() < kQuadBytes || !std::ranges::equal(bytes.first(kQuadBytes), written.quad)) {
        m_rewritten.fetch_add(1);
        return std::nullopt;
    }
    m_identified.fetch_add(1);
    return written.object;
}

} // namespace wiiuport::guest
