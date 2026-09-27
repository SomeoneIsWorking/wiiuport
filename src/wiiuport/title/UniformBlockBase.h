#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::title {

// The base the title's relative uniform-block offset is relative to.
//
// **This is the last thing standing between the objective's second question and an answer.** A
// binding's descriptor entry names its block by a *relative* offset: the word at `+0x04` is
// relative to a base the title set elsewhere, and the census's own `blockOf` says so and says
// that taking it for a length once "asked the product for a gigabyte and the product died". The
// block's SIZE is directly usable -- the word at `+0x0c` reads `0x40` for every object, which is
// 64 bytes -- but the ring test is a re-read of a *place*, and a relative offset is not a place.
//
// **The base is measured, not looked up.** The fork's uniform assembly hands over the real guest
// addresses of the blocks a draw sourced, as `(bufferId, physicalAddress)` pairs, in the same
// draw and immediately after the binder named the offset. So for every such address,
// `address - offset` is a candidate base, and the base the title actually uses is the one that
// recurs across hundreds of thousands of bindings. The report is a histogram with counts and a
// share, because a base that recurs 4 times out of 400,000 is not a base and a report that named
// it anyway would be a guess with a number on it.
//
// **The pairs are not matched by identity, and that is deliberate.** Nothing says which of a
// draw's several blocks is the one the binder named, so every address contributes a candidate.
// That over-generates rather than under-generates, and the histogram is the thing that
// separates a real base from the noise of the wrong pairings: a true base appears once per
// binding no matter which pairing produced it, and a wrong one does not repeat.
class UniformBlockBase {
  public:
    // A binding: the relative offset and the size the descriptor entry named.
    void publish(uint32_t object, uint32_t offset, uint32_t sizeInBytes);

    // The draw's real block addresses, as `(bufferId, physicalAddress)` pairs. Called on the
    // assembly that follows the binding, in the same draw.
    void observe(const std::vector<uint32_t>& blockSources);

    std::string json() const;

    struct Tally {
        uint64_t bindings = 0;
        uint64_t assemblies = 0;
        uint64_t assembliesWithABinding = 0;
        uint64_t candidates = 0;
        uint64_t pendingOverwritten = 0;
    };

    Tally tally() const;

    // The base, when one recurs often enough to be called one. Zero otherwise, and the report
    // says how near the top one came, so "not identified" is a measurement with a margin rather
    // than an absence.
    uint32_t bestBase(uint64_t* count) const;

  private:
    // The same, with the lock already held. `json()` holds it and needs the number, and a
    // non-recursive mutex taken twice on one thread is a deadlock rather than an answer -- which
    // is what the first version of this did, and it hung the test run.
    uint32_t bestBaseLocked(uint64_t* count) const;

  public:
    // **How many times the leading candidate must appear, as a share of the candidates
    // generated.** Generous on purpose: every address in every draw contributes a candidate, so
    // most candidates are the wrong pairing. The bar is not "the largest" -- that is a guess with
    // a number on it -- but "a large share of a corpus that is mostly wrong", which is a
    // property only a real base has.
    static constexpr double kBeliefShare = 0.20;
    // How many distinct bases the report names, most-recurring first.
    static constexpr size_t kExamples = 8;

  private:
    mutable std::mutex m_mutex;
    // The most recent binding, for the assembly that follows it in the same draw. One slot and
    // not a queue: the assembly immediately follows its own binding, and a queue would pair an
    // assembly with a binding from a draw that had already finished.
    bool m_pending = false;
    uint32_t m_pendingObject = 0;
    uint32_t m_pendingOffset = 0;
    uint32_t m_pendingSize = 0;
    // Base -> how many times it was generated. Bounded, and the overflow counted: an unbounded
    // map of every address minus every offset is a map of the whole address space.
    std::map<uint32_t, uint64_t> m_bases;
    uint64_t m_basesRefused = 0;
    static constexpr size_t kMaxBases = 4096;

    std::atomic<uint64_t> m_bindings{0};
    std::atomic<uint64_t> m_assemblies{0};
    std::atomic<uint64_t> m_assembliesWithABinding{0};
    std::atomic<uint64_t> m_candidates{0};
    std::atomic<uint64_t> m_pendingOverwritten{0};
};

} // namespace wiiuport::title
