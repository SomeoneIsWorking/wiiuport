#include "wiiuport/interp/KeyedFrame.h"

#include "wiiuport/interp/Blendable.h"

#include <algorithm>
#include <limits>

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
    m_byShader.resize(m_keys.size());
    for (uint32_t entry = 0; entry < m_keys.size(); ++entry) {
        m_byShader[entry] = entry;
    }
    std::stable_sort(m_byShader.begin(), m_byShader.end(), [this](uint32_t l, uint32_t r) {
        return m_keys[l].shader < m_keys[r].shader;
    });
    m_runs.clear();
    for (uint32_t begin = 0; begin < m_byShader.size();) {
        uint32_t end = begin + 1;
        while (end < m_byShader.size() &&
               m_keys[m_byShader[end]].shader == m_keys[m_byShader[begin]].shader) {
            ++end;
        }
        m_runs.push_back(order(begin, end));
        begin = end;
    }
}

KeyedFrame::ShaderRun KeyedFrame::order(uint32_t begin, uint32_t end) {
    ShaderRun run{m_keys[m_byShader[begin]].shader, begin, begin, end, ShaderDraws::kNoPosition};
    std::span<uint32_t> entries(m_byShader.data() + begin, end - begin);
    size_t common = std::numeric_limits<size_t>::max();
    for (uint32_t entry : entries) {
        common = std::min<size_t>(common, m_spans[entry].second);
    }
    // The widest spread tells the most draws apart; one that does not spread
    // them at all tells none apart, and the run stays unordered.
    float widest = 0.0f;
    for (uint32_t position = 0; position < common; ++position) {
        float low = std::numeric_limits<float>::infinity();
        float high = -low;
        for (uint32_t entry : entries) {
            float value = values(entry)[position];
            if (isNumber(value)) {
                low = std::min(low, value);
                high = std::max(high, value);
            }
        }
        if (high - low > widest) {
            widest = high - low;
            run.position = position;
        }
    }
    if (run.position == ShaderDraws::kNoPosition) {
        return run;
    }
    auto at = [this, position = run.position](uint32_t entry) {
        return values(entry)[position];
    };
    auto numbered = std::stable_partition(entries.begin(), entries.end(), [&](uint32_t entry) {
        return isNumber(at(entry));
    });
    std::stable_sort(entries.begin(), numbered, [&](uint32_t l, uint32_t r) {
        return at(l) < at(r);
    });
    run.orderedEnd = begin + static_cast<uint32_t>(numbered - entries.begin());
    return run;
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

KeyedFrame::ShaderDraws KeyedFrame::drawnBy(const ShaderKey& shader) const {
    auto run = std::lower_bound(m_runs.begin(), m_runs.end(), shader,
                                [](const ShaderRun& l, const ShaderKey& r) {
                                    return l.shader < r;
                                });
    if (run == m_runs.end() || run->shader != shader) {
        return {};
    }
    const uint32_t* entries = m_byShader.data();
    return {run->position,
            {entries + run->begin, entries + run->orderedEnd},
            {entries + run->orderedEnd, entries + run->end}};
}

std::span<const uint32_t> KeyedFrame::within(const ShaderDraws& draws, double low,
                                             double high) const {
    auto at = [this, position = draws.position](uint32_t entry) {
        return static_cast<double>(values(entry)[position]);
    };
    auto first =
        std::partition_point(draws.ordered.begin(), draws.ordered.end(), [&](uint32_t entry) {
            return at(entry) < low;
        });
    auto last = std::partition_point(first, draws.ordered.end(), [&](uint32_t entry) {
        return at(entry) <= high;
    });
    return {first, last};
}

bool KeyedFrame::sourced(uint32_t address) const {
    return std::binary_search(m_addresses.begin(), m_addresses.end(), address);
}

} // namespace wiiuport::interp
