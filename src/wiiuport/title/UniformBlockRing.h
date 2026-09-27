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

    // One binding: which object, which block, and what was in it. Called by the census on the
    // display thread, so the counters are atomic and the shared state is under the lock.
    void bind(uint32_t object, uint32_t address, uint32_t sizeInBytes);

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
    std::atomic<uint64_t> m_bindings{0};
    std::atomic<uint64_t> m_oversize{0};
    std::atomic<uint64_t> m_unreadable{0};
    std::atomic<uint64_t> m_refused{0};
};

} // namespace wiiuport::title
