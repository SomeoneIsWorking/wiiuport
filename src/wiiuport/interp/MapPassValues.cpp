#include "wiiuport/interp/MapPassValues.h"

#include "wiiuport/interp/Blendable.h"

namespace wiiuport::interp {

void MapPassValues::clear() {
    m_moved.clear();
}

void MapPassValues::add(uint64_t object, std::span<const float> twoBack,
                        std::span<const float> latest) {
    for (size_t index = 0; index < latest.size() && index < twoBack.size(); ++index) {
        if (sameBits(twoBack[index], latest[index])) {
            continue;
        }
        auto [held, first] = m_moved.try_emplace(endsOf(twoBack[index], latest[index]),
                                                 Holder{.object = object, .several = false});
        if (!first && held->second.object != object) {
            held->second.several = true;
        }
    }
}

bool MapPassValues::movesOnlyThePass(std::span<const float> twoBack,
                                     std::span<const float> latest) const {
    bool moved = false;
    for (size_t index = 0; index < latest.size() && index < twoBack.size(); ++index) {
        if (sameBits(twoBack[index], latest[index])) {
            continue;
        }
        auto found = m_moved.find(endsOf(twoBack[index], latest[index]));
        if (found == m_moved.end() || !found->second.several) {
            return false;
        }
        moved = true;
    }
    return moved;
}

size_t MapPassValues::holdThePass(std::span<const float> twoBack, std::span<const float> latest,
                                  std::span<float> blended) const {
    size_t held = 0;
    for (size_t index = 0; index < latest.size() && index < twoBack.size(); ++index) {
        if (sameBits(twoBack[index], latest[index])) {
            continue;
        }
        auto found = m_moved.find(endsOf(twoBack[index], latest[index]));
        if (found != m_moved.end() && found->second.several) {
            blended[index] = latest[index];
            ++held;
        }
    }
    return held;
}

} // namespace wiiuport::interp
