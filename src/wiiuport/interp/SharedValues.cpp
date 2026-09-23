#include "wiiuport/interp/SharedValues.h"

#include "wiiuport/interp/Midpoint.h"

#include <algorithm>
#include <bit>

namespace wiiuport::interp {

uint64_t SharedValues::endsOf(float twoBack, float latest) {
    return (uint64_t{std::bit_cast<uint32_t>(twoBack)} << 32) | std::bit_cast<uint32_t>(latest);
}

void SharedValues::clear() {
    m_wanted.clear();
    m_held.clear();
}

void SharedValues::addWanted(std::span<const float> twoBack, std::span<const float> latest) {
    size_t common = std::min(twoBack.size(), latest.size());
    for (size_t position = 0; position < common; ++position) {
        if (Midpoint::movedIn(twoBack[position], latest[position])) {
            m_wanted.insert(endsOf(twoBack[position], latest[position]));
        }
    }
}

void SharedValues::addBlended(std::span<const float> twoBack, std::span<const float> latest,
                              std::span<const float> blended) {
    size_t common = std::min({twoBack.size(), latest.size(), blended.size()});
    for (size_t position = 0; position < common; ++position) {
        if (!Midpoint::movedIn(twoBack[position], latest[position])) {
            continue;
        }
        uint64_t ends = endsOf(twoBack[position], latest[position]);
        if (!m_wanted.contains(ends)) {
            continue;
        }
        uint32_t drawn = std::bit_cast<uint32_t>(blended[position]);
        auto [held, fresh] = m_held.try_emplace(ends, Held{drawn, true});
        if (!fresh && held->second.blended != drawn) {
            held->second.agreed = false;
        }
    }
}

std::optional<float> SharedValues::blendOf(float twoBack, float latest) const {
    auto found = m_held.find(endsOf(twoBack, latest));
    if (found == m_held.end() || !found->second.agreed) {
        return std::nullopt;
    }
    return std::bit_cast<float>(found->second.blended);
}

} // namespace wiiuport::interp
