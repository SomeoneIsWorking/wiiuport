#include "wiiuport/title/UniformBlockAddress.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <utility>

namespace wiiuport::title {

void UniformBlockAddress::publish(uint32_t object, std::span<const uint32_t> record) {
    m_bindings.fetch_add(1, std::memory_order_relaxed);
    if (record.size() > kMaxWords) {
        // A record longer than the words compared would be silently truncated into an answer that
        // looks whole. Refused instead, and the count says how often it came up.
        m_recordsRefused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::scoped_lock lock(m_mutex);
    auto held = std::find_if(m_records.begin(), m_records.end(), [object](const Held& seen) {
        return seen.object == object;
    });
    if (held == m_records.end()) {
        if (m_records.size() >= kMaxObjects) {
            // The least recently bound record gives up its slot. Dropped rather than refused
            // because a refusal is a binding that contributed nothing, and the count of those is
            // not a measurement of the title.
            const auto oldest = std::min_element(m_records.begin(), m_records.end(),
                                                 [](const Held& left, const Held& right) {
                                                     return left.sequence < right.sequence;
                                                 });
            m_records.erase(oldest);
            m_recordsEvicted.fetch_add(1, std::memory_order_relaxed);
        }
        m_records.push_back(Held{});
        held = std::prev(m_records.end());
        held->object = object;
    }
    held->sequence = ++m_sequence;
    held->count = record.size();
    for (size_t word = 0; word < record.size(); word++) {
        held->words[word] = record[word];
    }
    held->quoted.assign(record.begin(), record.end());
}

bool UniformBlockAddress::holdsAddress(const std::vector<uint32_t>& pairs, uint32_t value) {
    for (size_t index = 1; index < pairs.size(); index += 2) {
        if (pairs[index] == value) {
            return true;
        }
    }
    return false;
}

void UniformBlockAddress::countKey(Candidate key) {
    // The mutex is already held: this is called from inside `observe`'s paired section.
    const auto seen = m_deltas.find(key);
    if (seen != m_deltas.end()) {
        const uint64_t before = seen->second;
        m_deltasByCount.erase(ByCount{before, key});
        seen->second = before + 1;
        m_deltasByCount.emplace(ByCount{before + 1, key}, 0);
        return;
    }
    if (m_deltas.size() < kMaxDeltas) {
        m_deltas.emplace(key, 1);
        m_deltasByCount.emplace(ByCount{1, key}, 0);
        return;
    }
    // Full: the least frequent entry gives up its slot. Counted, because a sketch's counts are
    // upper bounds on the truth and a reader has to know how much of the corpus was displaced.
    const auto lowest = m_deltasByCount.begin();
    if (lowest == m_deltasByCount.end()) {
        return;
    }
    const uint64_t carried = lowest->first.first + 1;
    m_deltas.erase(lowest->first.second);
    m_deltasByCount.erase(lowest);
    m_deltas.emplace(key, carried);
    m_deltasByCount.emplace(ByCount{carried, key}, 0);
    m_deltasEvicted++;
}

void UniformBlockAddress::observe(uint32_t object, const std::vector<uint32_t>& blockSources) {
    m_assemblies.fetch_add(1, std::memory_order_relaxed);
    std::array<uint32_t, kMaxWords> record{};
    size_t words = 0;
    {
        std::scoped_lock lock(m_mutex);
        const auto held =
            std::find_if(m_records.begin(), m_records.end(), [object](const Held& seen) {
                return seen.object == object;
            });
        if (held == m_records.end() || blockSources.size() < 2) {
            return;
        }
        record = held->words;
        words = held->count;
    }
    m_assembliesWithARecord.fetch_add(1, std::memory_order_relaxed);
    m_addressCount.fetch_add(blockSources.size() / 2, std::memory_order_relaxed);
    m_wordComparisons.fetch_add(words, std::memory_order_relaxed);

    std::scoped_lock lock(m_mutex);
    for (size_t index = 1; index < blockSources.size(); index += 2) {
        const uint32_t address = blockSources[index];
        auto found = m_addresses.find(address);
        if (found == m_addresses.end()) {
            if (m_addresses.size() >= kMaxAddresses) {
                m_addressesRefused++;
            } else {
                m_addresses.emplace(address, 1);
            }
        } else {
            found->second++;
        }
        // `address - word`, per word. If one word of the record is an offset into a pool, the
        // difference is that pool's base and it recurs; if a word is the address itself, the
        // difference is zero and the membership test below says so instead.
        //
        // Counted as a space-saving sketch: a bounded map that keeps the *least* frequent entry's
        // slot for a newcomer, so a base that recurs in a large share of draws survives a corpus
        // of noise. The alternative -- a plain bounded map -- threw the whole tail away and could
        // not have named a base however real it was.
        for (size_t word = 0; word < words; word++) {
            countKey({word, address - record[word]});
        }
    }
    // Which word, if any, *is* the address. All seven compete on the same corpus, so a word that
    // hits in a majority of the paired draws is the one carrying it and a word that hits
    // occasionally is a coincidence with a name on it.
    for (size_t word = 0; word < words; word++) {
        if (holdsAddress(blockSources, record[word])) {
            m_wordHits[word]++;
        }
    }
}

UniformBlockAddress::Tally UniformBlockAddress::tally() const {
    Tally out;
    out.bindings = m_bindings.load();
    out.recordsRefused = m_recordsRefused.load();
    out.recordsEvicted = m_recordsEvicted.load();
    out.assemblies = m_assemblies.load();
    out.assembliesWithARecord = m_assembliesWithARecord.load();
    out.addresses = m_addressCount.load();
    out.wordComparisons = m_wordComparisons.load();
    return out;
}

int UniformBlockAddress::addressWordLocked() const {
    const uint64_t paired = m_assembliesWithARecord.load();
    if (paired == 0) {
        return -1;
    }
    for (size_t word = 0; word < kMaxWords; word++) {
        if (static_cast<double>(m_wordHits[word]) / static_cast<double>(paired) >= kWordShare) {
            return static_cast<int>(word);
        }
    }
    return -1;
}

int UniformBlockAddress::addressWord() const {
    std::scoped_lock lock(m_mutex);
    return addressWordLocked();
}

std::string UniformBlockAddress::json() const {
    const Tally t = tally();
    std::scoped_lock lock(m_mutex);
    uint64_t total = 0;
    for (const auto& [address, seen] : m_addresses) {
        total += seen;
    }
    std::vector<std::pair<uint32_t, uint64_t>> ordered(m_addresses.begin(), m_addresses.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) {
            return left.second > right.second;
        }
        return left.first < right.first;
    });

    const int word = addressWordLocked();
    const uint64_t paired = t.assembliesWithARecord;

    JsonBody body;
    body.number("bindings", t.bindings);
    body.number("recordsHeld", m_records.size());
    body.number("recordsRefused", t.recordsRefused);
    body.number("recordsEvicted", t.recordsEvicted);
    body.number("assemblies", t.assemblies);
    body.number("assembliesWithARecord", paired);
    body.number("addresses", t.addresses);
    body.number("distinctAddresses", m_addresses.size());
    body.number("addressesRefused", m_addressesRefused);
    body.raw("wordShare", JsonBody::real(kWordShare));

    // The words, with their hit counts, so the reader sees the contrast rather than a verdict.
    JsonBody words;
    uint64_t bestHits = 0;
    for (size_t index = 0; index < kMaxWords; index++) {
        bestHits = std::max(bestHits, m_wordHits[index]);
        if (m_wordHits[index] == 0) {
            continue;
        }
        JsonBody one;
        one.number("offset", index * 4);
        one.number("hits", m_wordHits[index]);
        one.raw("share", JsonBody::real(paired == 0 ? 0.0
                                                    : static_cast<double>(m_wordHits[index]) /
                                                          static_cast<double>(paired)));
        words.object(std::to_string(index), one.text());
    }
    body.object("wordHits", words.text());

    if (word < 0) {
        body.raw("addressWord", "null");
        body.raw("bestWordShare", paired == 0 ? "null"
                                              : JsonBody::real(static_cast<double>(bestHits) /
                                                               static_cast<double>(paired)));
    } else {
        body.number("addressWord", word);
        body.number("addressWordOffset", static_cast<uint32_t>(word) * 4);
        body.number("addressWordHits", m_wordHits[static_cast<size_t>(word)]);
    }

    // The address histogram, because a constant address is a fact about a distribution and a reader
    // has to be able to see whether the leader is every draw or merely most of them.
    JsonBody histogram;
    size_t shown = 0;
    for (const auto& [address, seen] : ordered) {
        if (shown >= kExamples) {
            break;
        }
        JsonBody one;
        one.number("address", address);
        one.number("seen", seen);
        one.raw("share",
                JsonBody::real(
                    total == 0 ? 0.0 : static_cast<double>(seen) / static_cast<double>(total)));
        histogram.object(std::to_string(shown), one.text());
        shown++;
    }
    body.object("addressesByValue", histogram.text());
    if (ordered.empty()) {
        body.raw("leadingAddress", "null");
    } else {
        body.number("leadingAddress", ordered.front().first);
        body.number("leadingAddressSeen", ordered.front().second);
        body.raw("leadingAddressShare",
                 JsonBody::real(total == 0 ? 0.0
                                           : static_cast<double>(ordered.front().second) /
                                                 static_cast<double>(total)));
    }

    // `address - word`, keyed by the word, which is the live route when no word *is* an address.
    // One histogram over both, so the word and the base it implies come out of the same
    // measurement rather than out of a second guess at which word was meant.
    std::vector<std::pair<std::pair<size_t, uint32_t>, uint64_t>> bases(m_deltas.begin(),
                                                                        m_deltas.end());
    std::sort(bases.begin(), bases.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) {
            return left.second > right.second;
        }
        return left.first < right.first;
    });
    uint64_t baseTotal = 0;
    for (const auto& [key, seen] : m_deltas) {
        baseTotal += seen;
    }
    const bool named =
        !bases.empty() &&
        static_cast<double>(bases.front().second) / static_cast<double>(baseTotal) >= kBaseShare;
    body.number("baseCandidates", baseTotal);
    body.number("distinctCandidates", m_deltas.size());
    // `evicted` and not `refused`: a sketch displaces the least frequent entry rather than
    // dropping the newcomer, so nothing is refused outright and the count says how much of the
    // corpus was displaced instead.
    body.number("candidatesEvicted", m_deltasEvicted);
    body.raw("baseShare", JsonBody::real(kBaseShare));
    if (named) {
        body.number("baseWord", bases.front().first.first);
        body.number("baseWordOffset", static_cast<uint32_t>(bases.front().first.first) * 4);
        body.number("base", bases.front().first.second);
        body.number("baseSeen", bases.front().second);
        body.raw("baseSeenShare", JsonBody::real(static_cast<double>(bases.front().second) /
                                                 static_cast<double>(baseTotal)));
    } else {
        body.raw("base", "null");
        body.raw("baseSeenShare",
                 baseTotal == 0 ? "null"
                                : JsonBody::real(static_cast<double>(
                                                     bases.empty() ? 0 : bases.front().second) /
                                                 static_cast<double>(baseTotal)));
    }
    JsonBody shown2;
    size_t index2 = 0;
    for (const auto& [key, seen] : bases) {
        if (index2 >= kExamples) {
            break;
        }
        JsonBody one;
        one.number("word", key.first);
        one.number("wordOffset", key.first * 4);
        one.number("base", key.second);
        one.number("seen", seen);
        one.raw("share", JsonBody::real(baseTotal == 0 ? 0.0
                                                       : static_cast<double>(seen) /
                                                             static_cast<double>(baseTotal)));
        shown2.object(std::to_string(index2), one.text());
        index2++;
    }
    body.object("candidatesByBase", shown2.text());

    // The records themselves, so the words above can be checked against something that was read
    // rather than against this file's conclusion about them.
    JsonBody samples;
    for (size_t index = 0; index < m_records.size() && index < kSampleRecords; index++) {
        JsonBody one;
        one.number("object", m_records[index].object);
        std::string joined = "[";
        for (size_t wordIndex = 0; wordIndex < m_records[index].quoted.size(); wordIndex++) {
            if (wordIndex != 0) {
                joined += ",";
            }
            joined += std::to_string(m_records[index].quoted[wordIndex]);
        }
        joined += "]";
        one.raw("words", joined);
        samples.object(std::to_string(index), one.text());
    }
    body.object("sampleRecords", samples.text());
    return body.finish();
}

} // namespace wiiuport::title
