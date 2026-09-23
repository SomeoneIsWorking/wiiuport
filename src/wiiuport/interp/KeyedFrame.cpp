#include "wiiuport/interp/KeyedFrame.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace wiiuport::interp {

void KeyedFrame::OccurrenceTable::reset() {
    m_used = 0;
    ++m_stamp;
    if (m_stamp == 0) {
        // Wrapped: every stale slot would read as live.
        for (Slot& slot : m_slots) {
            slot.stamp = 0;
        }
        m_stamp = 1;
    }
}

void KeyedFrame::OccurrenceTable::grow() {
    std::vector<Slot> old = std::move(m_slots);
    m_slots.assign(std::max<size_t>(64, old.size() * 2), Slot{});
    size_t mask = m_slots.size() - 1;
    for (const Slot& slot : old) {
        if (slot.stamp != m_stamp) {
            continue;
        }
        size_t at = slot.hash & mask;
        while (m_slots[at].stamp == m_stamp) {
            at = (at + 1) & mask;
        }
        m_slots[at] = slot;
    }
}

uint32_t KeyedFrame::OccurrenceTable::next(const std::vector<AssemblyKey>& keys, uint32_t entry) {
    if ((m_used + 1) * 2 > m_slots.size()) {
        grow();
    }
    const AssemblyKey& key = keys[entry];
    uint64_t hash = key.drawHash();
    size_t mask = m_slots.size() - 1;
    for (size_t at = hash & mask;; at = (at + 1) & mask) {
        Slot& slot = m_slots[at];
        if (slot.stamp != m_stamp) {
            slot = Slot{hash, entry, 1, m_stamp};
            ++m_used;
            return 0;
        }
        if (slot.hash == hash && keys[slot.entry].sameDrawAs(key)) {
            return slot.count++;
        }
    }
}

void KeyedFrame::begin() {
    m_keys.clear();
    m_floats.clear();
    m_spans.clear();
    m_addresses.clear();
    m_byHash.clear();
    m_byShader.clear();
    m_indexed = false;
    m_occurrences.reset();
}

size_t KeyedFrame::add(const frame::RecordedUniformAssembly& assembly, const KeyedFrame* twoBack) {
    auto entry = static_cast<uint32_t>(m_keys.size());
    ShaderKey shader{assembly.shaderBaseHash, assembly.shaderAuxHash, assembly.stageIndex};
    AssemblyKey& key = m_keys.emplace_back(shader, assembly.blockSources);
    for (size_t word = 1; word < key.sourceCount; word += 2) {
        m_addresses.push_back(key.sources[word]);
        if (twoBack != nullptr && !twoBack->sourced(key.sources[word])) {
            key.sources[word] = AssemblyKey::kFreshBlock;
        }
    }
    key.occurrence = m_occurrences.next(m_keys, entry);
    m_spans.emplace_back(static_cast<uint32_t>(m_floats.size()),
                         static_cast<uint32_t>(assembly.data.size()));
    m_floats.insert(m_floats.end(), assembly.data.begin(), assembly.data.end());
    return entry;
}

void KeyedFrame::finish() {
    std::sort(m_addresses.begin(), m_addresses.end());
    m_addresses.erase(std::unique(m_addresses.begin(), m_addresses.end()), m_addresses.end());
    m_byHash.clear();
    for (uint32_t entry = 0; entry < m_keys.size(); ++entry) {
        m_byHash.emplace_back(m_keys[entry].hash(), entry);
    }
    std::sort(m_byHash.begin(), m_byHash.end());
}

void KeyedFrame::indexValues() {
    m_byShader.resize(m_keys.size());
    for (uint32_t entry = 0; entry < m_keys.size(); ++entry) {
        m_byShader[entry] = entry;
    }
    std::stable_sort(m_byShader.begin(), m_byShader.end(), [this](uint32_t l, uint32_t r) {
        return m_keys[l].shader < m_keys[r].shader;
    });
    m_groups.clear();
    m_shared.clear();
    m_tree.clear();
    DrawValues drawn = values();
    for (uint32_t begin = 0; begin < m_byShader.size();) {
        uint32_t end = begin + 1;
        while (end < m_byShader.size() &&
               m_keys[m_byShader[end]].shader == m_keys[m_byShader[begin]].shader) {
            ++end;
        }
        std::span<const uint32_t> group(m_byShader.data() + begin, end - begin);
        auto sharedBegin = static_cast<uint32_t>(m_shared.size());
        flagShared(group);
        m_groups.push_back(Group{m_keys[m_byShader[begin]].shader, m_tree.build(group, drawn),
                                 sharedBegin,
                                 static_cast<uint32_t>(m_shared.size()) - sharedBegin});
        begin = end;
    }
    m_indexed = true;
}

std::optional<size_t> KeyedFrame::find(const AssemblyKey& key) const {
    uint64_t hash = key.hash();
    auto first = std::lower_bound(m_byHash.begin(), m_byHash.end(), std::make_pair(hash, 0u));
    for (auto at = first; at != m_byHash.end() && at->first == hash; ++at) {
        if (m_keys[at->second] == key) {
            return at->second;
        }
    }
    return std::nullopt;
}

void KeyedFrame::flagShared(std::span<const uint32_t> group) {
    if (group.size() < 2) {
        return;
    }
    size_t common = values(group[0]).size();
    for (uint32_t entry : group) {
        common = std::min(common, values(entry).size());
    }
    std::span<const float> first = values(group[0]);
    for (size_t position = 0; position < common; ++position) {
        bool shared = std::all_of(group.begin(), group.end(), [&](uint32_t entry) {
            return std::memcmp(&values(entry)[position], &first[position], sizeof(float)) == 0;
        });
        m_shared.push_back(shared ? 1 : 0);
    }
}

const KeyedFrame::Group* KeyedFrame::group(const ShaderKey& shader) const {
    if (!m_indexed) {
        throw std::logic_error("a frame's draws were searched before its values were indexed");
    }
    auto found = std::lower_bound(m_groups.begin(), m_groups.end(), shader,
                                  [](const Group& l, const ShaderKey& r) {
                                      return l.shader < r;
                                  });
    if (found == m_groups.end() || found->shader != shader) {
        return nullptr;
    }
    return &*found;
}

DrawTree::Nearest KeyedFrame::nearest(const ShaderKey& shader, const DrawTree::Query& query) const {
    const Group* found = group(shader);
    if (found == nullptr) {
        return {};
    }
    return m_tree.nearest(found->tree, values(), query);
}

std::span<const uint8_t> KeyedFrame::sharedValues(const ShaderKey& shader) const {
    const Group* found = group(shader);
    if (found == nullptr) {
        return {};
    }
    return {m_shared.data() + found->sharedBegin, found->sharedCount};
}

bool KeyedFrame::sourced(uint32_t address) const {
    return std::binary_search(m_addresses.begin(), m_addresses.end(), address);
}

} // namespace wiiuport::interp
