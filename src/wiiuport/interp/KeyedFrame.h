#pragma once

#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/interp/AssemblyKey.h"
#include "wiiuport/interp/DrawTree.h"

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
    // frame is: lookups by key or address are only valid after finish(), and
    // searches by values after indexValues().
    void begin();
    // Returns the entry the assembly was given. Against `twoBack`, the frame
    // two before, a block it did not source is keyed as fresh; without one
    // every block is keyed by its address.
    size_t add(const frame::RecordedUniformAssembly& assembly, const KeyedFrame* twoBack);
    void finish();
    // Builds what nearest() searches. Apart from finish() because nothing
    // searches a frame until the next one is drawn, so it need not be built
    // while the title waits for the frame to end.
    void indexValues();

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

    // The draw of `shader` nearest the query's point, as DrawTree finds it;
    // none when the frame has no draw of that shader. Throws before
    // indexValues(): the tree would still be some earlier frame's.
    DrawTree::Nearest nearest(const ShaderKey& shader, const DrawTree::Query& query) const;

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

    DrawValues values() const {
        return {m_floats, m_spans};
    }

    std::vector<AssemblyKey> m_keys;
    std::vector<float> m_floats;
    std::vector<std::pair<uint32_t, uint32_t>> m_spans;
    // (key hash, entry), sorted.
    std::vector<std::pair<uint64_t, uint32_t>> m_byHash;

    // Entries grouped by shader, for building each shader's tree.
    std::vector<uint32_t> m_byShader;
    // Each shader's group in m_tree, sorted by shader.
    std::vector<std::pair<ShaderKey, uint32_t>> m_groups;
    DrawTree m_tree;
    bool m_indexed{false};
    // Sorted and unique.
    std::vector<uint32_t> m_addresses;
    OccurrenceTable m_occurrences;
};

} // namespace wiiuport::interp
