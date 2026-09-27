#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::title {

// Whether tick N-1's uniform block contents are still there when tick N paints.
//
// **The objective's own second question, and it is answerable without knowing where the pose
// is.** It asks whether the previous tick's uniform block survives to the moment the next tick
// paints, and the answer decides whether *any* blend could read it. A mechanism that needs tick
// N-1's pose at tick N's draw has to find those bytes still resident; a title that overwrites
// them in place has no such place to read, and a mechanism that assumed one would be reading
// whatever is there now.
//
// **The measurement is a re-read, not an inference.** At each binding the census hands over the
// block's guest address, its size and a hash of its bytes. When the *next* binding of the same
// object arrives, the earlier address is read again and compared with the earlier hash. If the
// bytes still hash the same, tick N-1's contents are still present at that address when tick N
// paints. If they do not, the address was overwritten, and the answer for that pair is no.
//
// The addresses themselves are reported too, because a title that alternates between two
// addresses for one object is double-buffering -- a ring -- and a ring is exactly the structure
// that *does* preserve the previous tick. So `addresses` per object and whether consecutive
// samples differ in address is in the report, and "the ring exists" is a measurement rather than
// an assumption about GX2.
class UniformBlockRing {
  public:
    // The guest read, the same seam the census and the pose history use: one caller, one read.
    using ReadWords = bool (*)(uint32_t guestAddress, uint32_t* values, uint32_t count);
    // The frame counter, so a pair is a pair of *ticks* and not two binds in one tick.
    using Frame = uint64_t (*)();

    // The counter, once the paint path exists to give one -- the same arrangement as the node
    // scan, because the paint mod is declared after this one.
    void setFrameCounter(const std::atomic<uint64_t>* counter) {
        m_frameCounter = counter;
    }

    UniformBlockRing(ReadWords readWords, Frame frame);

    // Where a block is and how big it is, as one value.
    //
    // **Not three adjacent `uint32_t`s.** `bind(object, address, size)` takes three of the same
    // type and `address` and `size` are both plausible small integers, so a transposed pair
    // compiles, runs, and reads a block of the wrong length at the wrong place -- which is exactly
    // what happened twice in this project's history with a neighbouring two-word call. A struct
    // makes the swap unrepresentable and names the two values at the call site.
    struct Block {
        uint32_t address = 0;
        uint32_t sizeInBytes = 0;
    };

    // One binding: which object, which block, and what was in it. Called by the census on the
    // display thread, so the counters are atomic and the shared state is under the lock.
    void bind(uint32_t object, Block block);

    // **What the title actually gives, and it is not an address.** The census reads two words
    // from a binding's descriptor entry and they are not both usable: the one at +0x0c is the
    // block's SIZE -- 0x40, which is 64 bytes, for every object -- and the one at +0x04 is a
    // *relative* offset, because this project's own census says so and says that reading it as
    // a length once asked the product for a gigabyte and the product died. The base the title
    // set elsewhere has not been identified, so a relative offset read as a guest address is a
    // wrong answer rather than a missing one.
    //
    // So the size is recorded and the address is not, and the report says which: `sizesKnown`
    // and `addressesKnown`. A ring test needs the address -- it is a re-read of a place -- and
    // until the base is found this class can say the block is 64 bytes and nothing more. That is
    // the honest state, and a run that reported a re-read against a relative offset would be
    // reporting a number about memory the title never named.
    void bindSize(uint32_t object, uint32_t sizeInBytes);

    std::string json() const;

    // The answer, with its denominator: how many consecutive pairs were re-read, how many still
    // held the previous tick's contents, and how many addresses the ring has.
    struct Tally {
        uint64_t objects = 0;
        uint64_t pairsCompared = 0;
        uint64_t stillPresent = 0;
        uint64_t overwritten = 0;
        uint64_t unreadable = 0;
        uint64_t refused = 0;
    };

    Tally tally() const;

    // The most bytes a block may be for this to mean anything. The measured size is 64 and the
    // bound is generous; a block past it is counted and not hashed, because a hash of a
    // megabyte per binding is a census that costs more than the frame it watches.
    static constexpr uint32_t kMaxBlockBytes = 4096;
    // How many objects are followed, and how many samples each keeps -- enough for one previous
    // tick per sample.
    static constexpr size_t kObjects = 8;
    static constexpr size_t kSamplesPerObject = 2;

  private:
    // One object's samples, oldest first.
    struct Sample {
        uint64_t frame = 0;
        uint32_t address = 0;
        uint32_t sizeInBytes = 0;
        uint64_t hash = 0;
        // Whether this sample HAD a predecessor to compare against, and whether the re-read of
        // that predecessor's address succeeded. Three states, not two: a first sample has
        // nothing to compare, a comparison that failed is not a "no", and a comparison that ran
        // is the only thing a count of overwrites may be built from. The first version had two
        // flags and could not tell a failed comparison from a first sample.
        bool compared = false;
        bool read = false;
        bool stillPresent = false;
        uint64_t previous = 0;
    };

    struct Tracked {
        uint32_t object = 0;
        std::vector<Sample> samples;
    };

    static uint64_t hashOf(const std::vector<uint32_t>& words);
    // FNV-1a over the block's words. Not a cryptographic hash and not meant to be: it answers
    // "are these the same bytes", and a collision would report two different blocks as one,
    // which the object count and the address list beside it would make visible.
    static constexpr uint64_t kFnvOffset = 14695981039346656037ull;
    static constexpr uint64_t kFnvPrime = 1099511628211ull;

    ReadWords m_readWords;
    Frame m_frame = nullptr;
    const std::atomic<uint64_t>* m_frameCounter = nullptr;
    mutable std::mutex m_mutex;
    std::vector<Tracked> m_tracked;
    // The block sizes the title named, which is the one half of the descriptor entry that is
    // usable without a base.
    std::vector<uint32_t> m_sizes = std::vector<uint32_t>(kObjects, 0);
    std::atomic<uint64_t> m_bindings{0};
    std::atomic<uint64_t> m_sizesKnown{0};
    // Bindings that carried an address this class was willing to re-read. Zero, and it is zero
    // for a reason: the title's address word is a relative offset whose base is not identified.
    std::atomic<uint64_t> m_addressesKnown{0};
    std::atomic<uint64_t> m_oversize{0};
    std::atomic<uint64_t> m_unreadable{0};
    std::atomic<uint64_t> m_refused{0};
};

} // namespace wiiuport::title
