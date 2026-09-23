#pragma once

#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/interp/AssemblyKey.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace wiiuport::interp {

// One recorded frame's uniform assemblies, indexed by the object each belongs
// to.
//
// Rebuilt every frame on the thread that draws, so it is built out of flat,
// sorted arrays whose capacity survives from one frame to the next: no
// allocation per draw and no hash table nodes. Pure, and knows nothing of
// blending.
class KeyedFrame {
  public:
    // Built one assembly at a time as the guest draws, then finished once the
    // frame is: lookups by key or shader are only valid after finish().
    void begin();
    // Returns the entry the assembly was given. Against `twoBack`, the frame
    // two before, a block it did not source is keyed as fresh; without one
    // every block is keyed by its address.
    size_t add(const frame::RecordedUniformAssembly& assembly, const KeyedFrame* twoBack);
    void finish();

    size_t size() const {
        return m_keys.size();
    }

    const AssemblyKey& key(size_t entry) const {
        return m_keys[entry];
    }

    std::span<const float> values(size_t entry) const {
        return {m_floats.data() + m_spans[entry].first, m_spans[entry].second};
    }

    std::optional<size_t> find(const AssemblyKey& key) const;

    // One shader's draws, ordered by their value at the one position that
    // spreads them widest, so a search can take only the draws near a value
    // there instead of all of them. Draws with no number at that position
    // cannot be ordered by it and are kept apart.
    struct ShaderDraws {
        static constexpr uint32_t kNoPosition = UINT32_MAX;
        uint32_t position{kNoPosition};
        std::span<const uint32_t> ordered;
        std::span<const uint32_t> unordered;
    };

    ShaderDraws drawnBy(const ShaderKey& shader) const;

    // Those of `draws.ordered` whose value at `draws.position` lies in
    // [low, high].
    std::span<const uint32_t> within(const ShaderDraws& draws, double low, double high) const;

    // Whether any draw of the frame sourced a block at this address, fresh
    // or not.
    bool sourced(uint32_t address) const;

  private:
    // Numbers each draw by how many identical draws the frame has already
    // made. Open addressing over a table that is kept between frames and
    // emptied by bumping a stamp, so numbering a frame allocates nothing once
    // the table has grown to fit one.
    class OccurrenceTable {
      public:
        void reset();
        uint32_t next(const std::vector<AssemblyKey>& keys, uint32_t entry);

      private:
        struct Slot {
            uint64_t hash{0};
            uint32_t entry{0};
            uint32_t count{0};
            uint32_t stamp{0};
        };

        void grow();

        std::vector<Slot> m_slots;
        // Slots start at 0, so a live stamp never is: a table used before its
        // first reset would otherwise read every empty slot as taken.
        uint32_t m_stamp{1};
        size_t m_used{0};
    };

    std::vector<AssemblyKey> m_keys;
    std::vector<float> m_floats;
    std::vector<std::pair<uint32_t, uint32_t>> m_spans;
    // (key hash, entry), sorted.
    std::vector<std::pair<uint64_t, uint32_t>> m_byHash;

    // One shader's run of m_byShader: [begin, orderedEnd) ordered by the
    // value at `position`, [orderedEnd, end) with no number there.
    struct ShaderRun {
        ShaderKey shader;
        uint32_t begin;
        uint32_t orderedEnd;
        uint32_t end;
        uint32_t position;
    };

    // Orders one shader's run by the position that spreads it widest.
    ShaderRun order(uint32_t begin, uint32_t end);

    // Entries grouped by shader, each group ordered as its run says.
    std::vector<uint32_t> m_byShader;
    // Sorted by shader.
    std::vector<ShaderRun> m_runs;
    // Sorted and unique.
    std::vector<uint32_t> m_addresses;
    OccurrenceTable m_occurrences;
};

} // namespace wiiuport::interp
