#include "wiiuport/interp/SharedValues.h"

#include "wiiuport/interp/Blendable.h"

#include <algorithm>
#include <bit>

namespace wiiuport::interp {

void SharedValues::clear() {
    m_held.clear();
}

void SharedValues::addBlended(const ShaderKey& shader, std::span<const float> twoBack,
                              std::span<const float> latest, std::span<const float> blended) {
    size_t common = std::min({twoBack.size(), latest.size(), blended.size()});
    for (size_t position = 0; position < common; ++position) {
        auto before = std::bit_cast<uint32_t>(twoBack[position]);
        auto after = std::bit_cast<uint32_t>(latest[position]);
        if (before == after || !isNumber(twoBack[position]) || !isNumber(latest[position])) {
            continue;
        }
        m_held.push_back(Held{shader, static_cast<uint32_t>(position), before, after,
                              std::bit_cast<uint32_t>(blended[position])});
    }
}

void SharedValues::index() {
    std::sort(m_held.begin(), m_held.end());
}

std::optional<float> SharedValues::blendOf(const ShaderKey& shader, uint32_t position,
                                           float twoBack, float latest) const {
    Held low{shader, position, std::bit_cast<uint32_t>(twoBack), std::bit_cast<uint32_t>(latest),
             0};
    Held high = low;
    high.blended = UINT32_MAX;
    auto first = std::lower_bound(m_held.begin(), m_held.end(), low);
    auto last = std::upper_bound(first, m_held.end(), high);
    // Sorted by what was drawn, so every holder agrees when the first and
    // last do.
    if (first == last || first->blended != std::prev(last)->blended) {
        return std::nullopt;
    }
    return std::bit_cast<float>(first->blended);
}

} // namespace wiiuport::interp
