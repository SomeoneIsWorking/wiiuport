#include "wiiuport/interp/AssemblyKey.h"

#include <algorithm>

namespace wiiuport::interp {

namespace {

uint64_t mix(uint64_t seed, uint64_t value) {
    // splitmix64's finaliser: every input bit reaches every output bit, so
    // addresses that differ only in low bits do not share a bucket.
    value += 0x9e3779b97f4a7c15ull + seed;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

} // namespace

AssemblyKey::AssemblyKey(const ShaderKey& drawnBy, std::span<const uint32_t> sourceWords)
    : shader(drawnBy),
      sourceCount(static_cast<uint32_t>(std::min(sourceWords.size(), kMaxSourceWords))) {
    std::copy_n(sourceWords.begin(), sourceCount, sources.begin());
}

bool AssemblyKey::sameDrawAs(const AssemblyKey& other) const {
    return shader == other.shader && sourceCount == other.sourceCount &&
           std::equal(sources.begin(), sources.begin() + sourceCount, other.sources.begin());
}

bool AssemblyKey::describes(const AssemblyKey& drawn) const {
    if (shader != drawn.shader || sourceCount != drawn.sourceCount) {
        return false;
    }
    for (size_t word = 0; word < sourceCount; ++word) {
        bool freshAddress = (word % 2) == 1 && sources[word] == kFreshBlock;
        if (!freshAddress && sources[word] != drawn.sources[word]) {
            return false;
        }
    }
    return true;
}

uint64_t AssemblyKey::drawHash() const {
    uint64_t seed = mix(mix(mix(0, shader.baseHash), shader.auxHash), shader.stageIndex);
    for (uint32_t word : sourceWords()) {
        seed = mix(seed, word);
    }
    return seed;
}

uint64_t AssemblyKey::hash() const {
    return mix(drawHash(), occurrence);
}

} // namespace wiiuport::interp
