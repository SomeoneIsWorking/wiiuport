#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace wiiuport::title {

// Where the uniform block a draw sources actually is, and which word of the descriptor record
// says so.
//
// **This replaces a base measurement, and the replacement is the finding.** The binder at
// 0x027ff88c / 0x027ff9c0 was decompiled to find the base its relative offset was relative to:
// `GX2SetVertexUniformBlock(iVar5, uVar4, uVar6)` with `uVar4 = record[0x0c]` and
// `uVar6 = record[0x04]`, and the fork's `_GX2SubmitUniformBlock` writes one of those two into
// the uniform block register as `memory_virtualToPhysical(...)` with **nothing added to it**. So
// the two words are the address and the size in one order or the other, and there is no base to
// find: the earlier reading of them was the difference of a size and an address, which is why
// `address - offset` histogrammed over 180,707 bindings produced no candidate above a 2.0% share.
//
// **The pairs are by the title's own object, not by adjacency.** The first version published the
// most recent record and paired it with the next assembly to arrive, which paired 77,274 of
// 855,599 -- a 9% lottery, where a wrong pairing lowers every word's hit rate equally and a right
// one is invisible. The assembly names the object the draw is in the middle of
// (`ObjectIdentityScope::current()`), and the binder named the same object, so the pair is exact.
// Identity here is the title's own, read from its own binder; nothing is matched by occurrence
// index, by address similarity, or by a host-side guess.
//
// **Two questions, one corpus.** *Is one of the record's words the address?* is a membership
// test: a word is the address if its value is one of the addresses the draw sourced, in a large
// share of the paired draws. *If none of them is, is one of them an offset into a pool?* is a
// histogram of `address - word` keyed by the word, so the offset word and the pool's base fall out
// of the same measurement rather than out of a second guess. Measured over 77,274 lottery pairings
// the first question came back **no word at all** -- so the histogram is the live route, and it is
// reported whether or not it finds anything.
class UniformBlockAddress {
  public:
    // The descriptor record, as the binder read it, in the title's own order. A span because the
    // record's size belongs to the census that read it, and a second copy of the stride here
    // would be one more thing to fall out of step with it.
    //
    // Held **by object**, not as the most recent one. An adjacency slot is a pairing that went
    // wrong 91% of the time and said nothing.
    void publish(uint32_t object, std::span<const uint32_t> record);

    // The draw's real block addresses, as `(bufferId, physicalAddress)` pairs, with the object the
    // draw is in the middle of. Called on the assembly, so the pair is the record of *that* object.
    void observe(uint32_t object, const std::vector<uint32_t>& blockSources);

    std::string json() const;

    struct Tally {
        uint64_t bindings = 0;
        uint64_t recordsRefused = 0;
        uint64_t recordsEvicted = 0;
        uint64_t assemblies = 0;
        uint64_t assembliesWithARecord = 0;
        uint64_t addresses = 0;
        uint64_t wordComparisons = 0;
    };

    Tally tally() const;

    // The record word that names one of the draw's real block addresses, or -1 when none does.
    int addressWord() const;

    // **How large a share of the paired draws a word must hit to be called the address.** Every
    // word is compared against the same address list, so the seven compete on one corpus and the
    // bar is a majority rather than a lead: a lead is a guess with a number on it.
    static constexpr double kWordShare = 0.5;
    // How many distinct addresses the address histogram keeps. Bounded, and the overflow counted.
    static constexpr size_t kMaxAddresses = 4096;
    // How many objects' records are held at once, and what happens when it is full: the
    // **least-recently-bound** record is dropped, because a pair only needs the record of the
    // object whose draw is about to happen and a title's binding order is a traversal. Refusing
    // instead threw away 179,285 of 388,000 publishes in one run -- 46% of the bindings contributed
    // nothing
    // -- and a bound of 64 was picked from a census that reported eight objects, which was eight
    // *drawn* objects and not eight bound ones.
    static constexpr size_t kMaxObjects = 4096;
    // How many distinct `(word, address - word)` pairs the histogram keeps. Bounded, because an
    // unbounded map of every address minus every word is a map of the address space -- and held as
    // a **space-saving sketch**, not a plain map, because a plain bounded map keeps the first
    // `kMaxDeltas` keys that arrive and evicts nothing. Measured: 1,441,075 candidates into 4,096
    // slots, so the whole tail was thrown away and a base that recurs in a large share of draws
    // could be among what was thrown. The sketch evicts the *least* frequent entry instead, which
    // is what makes a heavy hitter survive a corpus of noise, and the evictions are counted.
    static constexpr size_t kMaxDeltas = 65536;
    // How many distinct records quoted raw.
    static constexpr size_t kSampleRecords = 4;
    // How many distinct values the histogram names, most-recurring first.
    static constexpr size_t kExamples = 8;
    // How large a share of the corpus a `(word, base)` must reach to be named as the base. Generous
    // on purpose: every address in every draw contributes a candidate, so most candidates are the
    // wrong pairing. The bar is not "the largest" -- that is a guess with a number on it -- but a
    // large share of a corpus that is mostly wrong, which only a real base has.
    static constexpr double kBaseShare = 0.05;

  private:
    static constexpr size_t kMaxWords = 16;

    struct Held {
        uint32_t object = 0;
        // When this object was last bound, so the least recent is the one to drop when the bound is
        // reached. A counter rather than a clock: the display thread's own, and a record is only
        // useful immediately after it was written.
        uint64_t sequence = 0;
        std::array<uint32_t, kMaxWords> words{};
        size_t count = 0;
        std::vector<uint32_t> quoted;
    };

    int addressWordLocked() const;
    // One `(word, base)` candidate, into the sketch. The lock is already held.
    using Candidate = std::pair<size_t, uint32_t>;
    // `(count, candidate)`, so the least frequent candidate is the first entry.
    using ByCount = std::pair<uint64_t, Candidate>;
    void countKey(Candidate key);
    // `value` is in the `pairs` list as an address, i.e. at an odd index.
    static bool holdsAddress(const std::vector<uint32_t>& pairs, uint32_t value);

    mutable std::mutex m_mutex;
    // One held record per object, bounded. Re-publishing an object overwrites its own record,
    // which is the common case: the binder runs per object per draw.
    std::vector<Held> m_records;
    // `(word, address - word)` -> how many draws produced it, and the same keys ordered by count so
    // the least frequent one is the first. The pair of maps is the sketch: incrementing a key
    // touches both, and evicting is the front of the second.
    std::map<Candidate, uint64_t> m_deltas;
    std::map<ByCount, size_t> m_deltasByCount;
    uint64_t m_deltasEvicted = 0;
    // The address histogram, over every paired draw.
    std::map<uint32_t, uint64_t> m_addresses;
    uint64_t m_addressesRefused = 0;
    // Word index -> how many paired draws that word named one of the draw's addresses.
    std::array<uint64_t, kMaxWords> m_wordHits{};

    std::atomic<uint64_t> m_bindings{0};
    std::atomic<uint64_t> m_recordsRefused{0};
    std::atomic<uint64_t> m_recordsEvicted{0};
    uint64_t m_sequence = 0;
    std::atomic<uint64_t> m_assemblies{0};
    std::atomic<uint64_t> m_assembliesWithARecord{0};
    std::atomic<uint64_t> m_addressCount{0};
    std::atomic<uint64_t> m_wordComparisons{0};
};

} // namespace wiiuport::title
