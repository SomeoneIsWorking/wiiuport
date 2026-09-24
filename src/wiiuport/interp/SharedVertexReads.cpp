#include "wiiuport/interp/SharedVertexReads.h"

#include <algorithm>
#include <numeric>

namespace wiiuport::interp {
namespace {

// Meshes joined into groups, each named by its lowest mesh.
class MeshUnion {
  public:
    explicit MeshUnion(size_t meshCount) : m_parent(meshCount) {
        std::iota(m_parent.begin(), m_parent.end(), uint32_t{0});
    }

    uint32_t find(uint32_t mesh) {
        while (m_parent[mesh] != mesh) {
            m_parent[mesh] = m_parent[m_parent[mesh]];
            mesh = m_parent[mesh];
        }
        return mesh;
    }

    void join(uint32_t one, uint32_t other) {
        uint32_t a = find(one);
        uint32_t b = find(other);
        m_parent[std::max(a, b)] = std::min(a, b);
    }

  private:
    std::vector<uint32_t> m_parent;
};

// One run of overlapping reads: a mesh reading it, and whether a draw the
// replay cannot redirect reads it too.
struct ReadRun {
    std::optional<uint32_t> mesh;
    bool unredirected{false};
};

} // namespace

VertexReadGroups groupVertexReads(std::vector<VertexRead> reads, size_t meshCount) {
    std::erase_if(reads, [](const VertexRead& read) {
        return read.size == 0;
    });
    std::ranges::sort(reads, {}, &VertexRead::begin);
    MeshUnion meshes(meshCount);
    std::vector<uint32_t> unredirectedMeshes;
    ReadRun run;
    uintptr_t runEnd = 0;
    auto endRun = [&] {
        if (run.unredirected && run.mesh.has_value()) {
            unredirectedMeshes.push_back(*run.mesh);
        }
        run = ReadRun{};
    };
    for (const VertexRead& read : reads) {
        if (read.begin >= runEnd) {
            endRun();
        }
        runEnd = std::max(runEnd, read.begin + read.size);
        if (!read.mesh.has_value()) {
            run.unredirected = true;
        } else if (run.mesh.has_value()) {
            meshes.join(*run.mesh, *read.mesh);
        } else {
            run.mesh = read.mesh;
        }
    }
    endRun();
    VertexReadGroups groups{.groupOf = std::vector<uint32_t>(meshCount),
                            .readUnredirected = std::vector<uint8_t>(meshCount, 0)};
    for (uint32_t mesh = 0; mesh < meshCount; ++mesh) {
        groups.groupOf[mesh] = meshes.find(mesh);
    }
    for (uint32_t mesh : unredirectedMeshes) {
        groups.readUnredirected[meshes.find(mesh)] = 1;
    }
    for (uint32_t mesh = 0; mesh < meshCount; ++mesh) {
        groups.readUnredirected[mesh] = groups.readUnredirected[groups.groupOf[mesh]];
    }
    return groups;
}

} // namespace wiiuport::interp
