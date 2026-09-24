#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/interp/TransformSearch.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace wiiuport::interp {

// Which object one draw's uniform assembly belongs to.
//
// The guest's own storage is the identity: the shader, the (bufferId,
// address) pairs of the uniform blocks it sourced, and how many draws before
// it in the same frame had the same two. Draw order is not an identity --
// objects enter and leave, and every index after a removal names something
// else -- but the n-th draw of one object from one set of blocks is, because
// the engine issues an object's passes in its own fixed order.
//
// A block the title allocates afresh every frame -- measured: one of the three
// blocks the most-drawn world shader sources is at an address no frame of the
// four before used -- names no object, so it is keyed as fresh rather than by
// its address.
//
// Held inline, so a frame of keys is built without an allocation per draw.
struct AssemblyKey {
    static constexpr size_t kMaxSourceWords = size_t{LatteFrameHooks::kMaxUniformBlockSources} * 2;
    // Stands in for the address of a block no frame two back sourced.
    static constexpr uint32_t kFreshBlock = UINT32_MAX;

    ShaderKey shader;
    uint32_t sourceCount{0};
    std::array<uint32_t, kMaxSourceWords> sources{};
    uint32_t occurrence{0};

    // Words past the cap are the interface's to refuse, not this key's.
    AssemblyKey(const ShaderKey& drawnBy, std::span<const uint32_t> sourceWords);
    AssemblyKey() = default;

    std::span<const uint32_t> sourceWords() const {
        return {sources.data(), sourceCount};
    }

    // Everything but the occurrence: the same object's blocks, drawn again.
    bool sameDrawAs(const AssemblyKey& other) const;
    // Whether a draw keyed by its real addresses is this key's draw, a fresh
    // block standing for whatever address it had.
    bool describes(const AssemblyKey& drawn) const;

    bool operator==(const AssemblyKey& other) const {
        return occurrence == other.occurrence && sameDrawAs(other);
    }

    // Of everything but the occurrence, so equal draws hash together and the
    // occurrence can be numbered after the fact.
    uint64_t drawHash() const;
    uint64_t hash() const;
};

} // namespace wiiuport::interp
