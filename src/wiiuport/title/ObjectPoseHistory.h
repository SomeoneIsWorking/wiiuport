#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::title {

// Which of the title's uniform blocks carry a pose, and whether that pose is written
// before the block is bound.
//
// The per-object binder at `0x027ff88c` names a block and hands it to the GPU, once
// per object per stage, inside the title's own draw. That makes it the one place where
// both ends of an interpolation could be in memory at the same moment, and the question
// is whether they are:
//
//   - is the pose at `+0xc4` of the block written *before* the binder runs, or after?
//     Written before, the value at the binder is this tick's and the value kept from
//     the block's previous binding is the last tick's, so both are in memory at the
//     binding and the binder alone is enough to blend. Written after, the value at the
//     binder is the *previous* tick's and the current one has to be read where it is
//     written -- a different probe at a different address.
//
// Three things were measured about this and each changed the design:
//
// **The series is per block, not per object.** The binder's cursor alternates *per
// binding* -- 52,012 switches over 136,906 repeat bindings -- so an object's pose is in
// a different block at each binding. Keyed by object, every comparison is between two
// different blocks' readings and none of them is a comparison at all: measured, 0
// comparisons over 209,615 observations. Keyed by block, each block's readings are
// compared against that same block's previous reading, which is the only comparison that
// means anything. The *object* is still the identity a blend is keyed on; the block is
// what holds a pose, and those are two different facts.
//
// **Not every block carries a pose.** Of the first eight objects tracked, seven read
// zero at `+0xc4` and one read something that is not a rigid transform. So the tracker
// does not take the first objects it sees and call them poses: it *tests* each block's
// twelve floats for a rigid 3x4 -- three unit rows, mutually perpendicular -- and
// counts the ones that pass and the ones that do not. A block that fails is not an
// error, it is a block the blend cannot act on, and the report says how many of each.
//
// **The pose is not rigid on every block that has one.** So the tolerance is stated and
// the test is reported as a count, not as a yes.
class ObjectPoseHistory {
  public:
    // The pose, as located: twelve floats at `+0xc4` of a 256-byte block, three rows of
    // unit length and mutually perpendicular with a translation beside them, and a zero
    // fourth float in each group of four.
    static constexpr uint32_t kPoseOffset = 0xc4;
    static constexpr uint32_t kPoseWords = 12;
    // How long a row may be off unit length, and how far two rows may be off
    // perpendicular, before a block is said not to carry a pose. Loose enough for a
    // matrix built in single precision through a chain of transforms, and stated so
    // that a number near the edge is a number somebody can argue with.
    static constexpr float kUnitTolerance = 0.01f;
    static constexpr float kPerpendicularTolerance = 0.01f;
    // How many pose-carrying blocks are tracked, and how many readings each keeps.
    // Three is the minimum that can say "changed since the last time" and "changed every
    // time" as different findings.
    static constexpr size_t kBlocks = 8;
    static constexpr size_t kHistory = 3;
    // How many blocks the report names whole.
    static constexpr size_t kExamples = kBlocks;

    // A whole uniform block is 256 bytes -- the two slots of every object measured are
    // exactly `0x100` apart -- and the pose was located at one offset inside it. That
    // offset is checked against every 4-aligned offset in the block, for a few blocks,
    // because "the pose is at `+0xc4`" and "the block holds no pose at all when it is
    // bound" are different findings and only one of them is a wrong offset.
    //
    // Bounded, because the display thread pays for it: the first `kScanned` distinct
    // blocks are read whole at every one of their bindings and nothing after. A scan
    // over every offset of every block is a search, and a search in a measured run is
    // how a probe turns into a load.
    static constexpr uint32_t kBlockWords = 64;
    static constexpr size_t kScanned = 4;

    // The fork's seam, injected so this is testable without a guest, and the fork's own
    // shape so no adapter stands between them: `count` words from `guestAddress`, guest
    // order, nothing written unless the whole range reads.
    //
    // One call rather than twelve, because this runs on the display thread inside the
    // title's own draw, and twelve separate reads there is twelve chances to be slow in
    // the one place being measured. The size belongs to this class rather than to the
    // seam's type, so the twelve is a fact about the pose.
    using ReadWords = bool (*)(uint32_t guestAddress, uint32_t* values, uint32_t count);

    explicit ObjectPoseHistory(ReadWords readWords);

    // Called on the display thread once per binding, with the object and the two blocks
    // its descriptor names. Zero for either address is a descriptor that named nothing,
    // and is counted as such rather than read as a pose of zeroes.
    void observe(uint32_t object, uint32_t block, uint32_t otherBlock);

    std::string json() const;

    uint64_t observations() const {
        return m_observations;
    }

  private:
    // One block's readings, and what comparing them has said.
    struct Series {
        uint32_t block = 0;
        uint32_t object = 0;
        uint32_t otherBlock = 0;
        // The last few readings, newest at index 0, and how many are real.
        std::array<std::array<uint32_t, kPoseWords>, kHistory> history{};
        size_t held = 0;
        // Comparisons against this block's previous reading, and how many found a
        // changed float. Equal to the comparison count means nothing writes the pose
        // before binding; zero means it always does.
        uint64_t compared = 0;
        uint64_t changed = 0;
        uint64_t changedWords = 0;
        // The largest single-float difference seen, so "moved" carries a number.
        float biggestDelta = 0.0f;
        // How many bindings saw this block as the *other* slot, and how many of those
        // found the other slot holding what this block held at its previous binding.
        // That is the ring carrying the previous tick, asked as a comparison of two
        // readings rather than assumed from a cursor.
        uint64_t seenAsOther = 0;
        uint64_t otherMatched = 0;
    };

    // A pose is a rigid transform: three rows of unit length, mutually perpendicular,
    // with a translation beside them. Tested, not assumed, and the count of blocks that
    // pass is the report's answer to which blocks the blend can act on.
    static bool isPose(const std::array<uint32_t, kPoseWords>& words);
    bool readPose(uint32_t block, std::array<uint32_t, kPoseWords>& pose) const;

    ReadWords m_readWords;
    std::atomic<uint64_t> m_observations{0};
    mutable std::mutex m_mutex;
    std::vector<Series> m_series;
    uint64_t m_refused = 0;
    uint64_t m_unreadable = 0;
    uint64_t m_noObject = 0;
    uint64_t m_poseBlocks = 0;
    uint64_t m_notPoseBlocks = 0;
    // The offset scan: for each 4-aligned offset in the block, how many scanned
    // bindings held a rigid transform there. Zero everywhere is the finding that the
    // block does not hold a pose when it is bound.
    std::array<uint64_t, kBlockWords - 2> m_offsets{};
    std::vector<uint32_t> m_scanned;
    uint64_t m_scans = 0;
};

} // namespace wiiuport::title
