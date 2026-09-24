#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace wiiuport::interp {

// A range of guest memory one of a frame's draws reads vertices from.
struct VertexRead {
    uintptr_t begin{0};
    size_t size{0};
    // The mesh the vertex blend knows the draw by; none for a draw the replay
    // draws from the title's bytes, whose own uniforms it cannot tell apart.
    std::optional<uint32_t> mesh;
};

// Which of a frame's meshes read the same bytes.
//
// Every reader of a byte draws what it holds: a mesh blended while another
// reader of its bytes draws them as the title did is two meshes on screen,
// one half a frame from the other -- a toon outline showing black through the
// hair it wraps. Readers are grouped by overlapping ranges, since a title may
// draw one mesh's bytes from a pointer part way into them.
struct VertexReadGroups {
    // Each mesh's group, named by its lowest mesh; a mesh reading bytes no
    // other mesh reads is a group of its own.
    std::vector<uint32_t> groupOf;
    // Whether a draw the replay cannot redirect reads bytes of the mesh's
    // group: that draw is drawn as the title drew it, so the group must be.
    std::vector<uint8_t> readUnredirected;
};

VertexReadGroups groupVertexReads(std::vector<VertexRead> reads, size_t meshCount);

} // namespace wiiuport::interp
