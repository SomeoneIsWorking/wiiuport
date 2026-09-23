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
        if (m_wanted.contains(ends)) {
            m_held.push_back(Held{ends, std::bit_cast<uint32_t>(blended[position])});
        }
    }
}

void SharedValues::index() {
    std::sort(m_held.begin(), m_held.end());
}

std::optional<float> SharedValues::blendOf(float twoBack, float latest) const {
    uint64_t ends = endsOf(twoBack, latest);
    auto first = std::lower_bound(m_held.begin(), m_held.end(), Held{ends, 0});
    auto last = std::upper_bound(first, m_held.end(), Held{ends, UINT32_MAX});
    // Sorted by what was drawn, so every holder agrees when the first and
    // last do.
    if (first == last || first->blended != std::prev(last)->blended) {
        return std::nullopt;
    }
    return std::bit_cast<float>(first->blended);
}

} // namespace wiiuport::interp
