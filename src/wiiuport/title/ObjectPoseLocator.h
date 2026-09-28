#pragma once

#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/title/TransformShape.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::title {

// Where in an assembled uniform buffer the pose is, found by shape and then constant.
//
// The blend needs one number: the byte offset of an object's pose inside the uniform
// buffer the game assembled for its draw. The fork hands over every one of those buffers
// (`frame::RecordedUniformAssembly::data`) together with the guest blocks the draw sourced
// (`blockSources`), and the draw's own identity is the display list it belongs to -- the
// node's record, reached from `node + 0xa4`. So the pose is *in* these bytes; the only
// thing missing was where.
//
// It is found here, once, by the only test that says "this is a pose": three rows of unit
// length, mutually perpendicular, with a translation beside them. That is what makes
// twelve floats a transform rather than three rows of numbers that happen to be near unit
// length, and it is the same test the title's own evidence was put through. Every
// 4-aligned offset in every buffer is tested and the counts are kept, so the answer is
// "offset X held a transform in N of M assemblies" rather than "a transform was seen".
//
// **What the counts are for.** A hit at one offset in one assembly is noise: a colour
// triple can be near unit length by chance, and a matrix of ones is not orthonormal but
// its rows are equal. So an offset is only believed when it holds a transform *often*, and
// the report gives the assemblies each offset was seen in, out of the assemblies there
// were. The offsets that clear the bar are named; the ones that do not are counted, so
// "no offset was a pose" is a statement about a set rather than an absence.
//
// The identity is the block sources, not the draw's place in the frame: two assemblies
// with the same `blockSources` are the same object's, and that is what lets the locator
// say whether the value at a candidate offset *moves* between them. A rigid transform that
// never moves is a colour triple that happened to look like one.
class ObjectPoseLocator : public frame::AssemblyRecordedListener {
  public:
    // The tolerances and the two classes live in `TransformShape`, shared with the locator
    // that reads the node's own memory. Two copies of a classification is a classification
    // with two futures, and these two already disagreed: this one only ever counted a
    // *rigid* 3x4, so "no offset was a pose" here also covered every scaled one, and the
    // scaled ones are the ones that would be a pose with a scale on it.
    using Shape = TransformShape;
    static constexpr size_t kPoseWords = Shape::kWords;
    // The most bytes of an assembled buffer the scan will consider. A buffer larger than
    // this is reported as unscanned rather than scanned in part, because a partial scan's
    // "no pose here" is about the part it looked at and not about the buffer.
    static constexpr uint32_t kMaxScanBytes = 4096;
    // How many candidate offsets the report names, most assemblies first.
    static constexpr size_t kExamples = 8;
    // How many distinct block-source identities are followed, so "does it move" is asked
    // of a bounded set of objects rather than of every draw the title has ever made.
    static constexpr size_t kIdentities = 16;

    // A candidate offset, and what was seen there.
    struct Candidate {
        uint32_t offset = 0;
        uint64_t assemblies = 0;   // assemblies holding a transform here
        uint64_t compared = 0;     // repeat assemblies of one identity
        uint64_t moved = 0;        // of those, how many found the value moved
        uint64_t still = 0;        // of those, how many found it did not
        float biggestDelta = 0.0f; // the largest single-float change seen
        // Distinct identities that have held this offset. **This was a constant 1** -- set when the
        // candidate was created and never moved -- so a report sorting offsets by it sorted them by
        // nothing. It is now counted where the identity set is used, and the two differ whenever
        // more than one object has held the offset, which is the normal case.
        uint64_t identities = 0;
        // The two classes, counted apart. `affine` is the superset -- a non-singular 3x3 --
        // and `rigid` the strict one. An offset can be affine in every assembly and rigid in
        // none, and that difference is the answer to "is this a transform, or a rigid one".
        uint64_t affineAssemblies = 0;
        uint64_t rigidAssemblies = 0;
        // How far the row lengths are from 1, as a deviation. Zero is rigid.
        float biggestScale = 0.0f;

        // **Is the value the same in every object, or does it differ between them?** This is the
        // question that says whether an offset holds a *global* or a *per-object* pose, and it is
        // the question the blend turns on.
        //
        // A camera's view matrix is written once a frame and read by every shader, so two different
        // objects at the same offset hold the *same* twelve floats. A static prop's world matrix is
        // that object's own, so two objects at the same offset hold *different* ones. The share of
        // assemblies an offset appears in cannot tell those apart -- a quarter of the frame's draws
        // being objects fits a per-object pose exactly as well as it fits a global -- so the
        // discriminator is the value itself, compared across identities.
        //
        // **Compared only across different identities, and against the value seen most recently.**
        // Two rules, and the second one was got wrong first.
        //
        // Within one object the value is expected to move between ticks, so comparing an object
        // against itself would count every per-object pose as one that differs from itself. Hence
        // the identity set.
        //
        // **The value compared against is the last one seen, not the first ever.** A camera's view
        // matrix changes every frame, so retaining the first value would make every object from
        // the second frame onwards "differ" from it -- and the answer would be the opposite of the
        // truth, from an instrument that looked right. What is asked is "does this object hold the
        // same value as the one before it", which is a question about *consecutive* assemblies, and
        // the consecutive assembly is the last one.
        uint32_t otherIdentities = 0;
        uint32_t otherIdentitiesSame = 0;
        uint32_t otherIdentitiesDifferent = 0;
        std::array<float, kPoseWords> lastValue{};
        bool hasLastValue = false;
        // The identities already compared against the value most recently seen, so a repeat of one
        // of them is not counted again. Bounded, and the bound is reported: past it the count stops
        // rather than growing with the frame, and a report that said "compared against 4 objects"
        // would be true while "compared against every object" would not be.
        std::vector<std::string> identitiesSeen;
        bool identitySamplesCapped = false;
    };

    void onAssemblyRecorded(const frame::RecordedUniformAssembly& assembly) override;

    std::string json() const;

    uint64_t assembliesSeen() const {
        return m_assemblies;
    }

    // The offset the report believes, or 0 when no offset cleared the bar. Zero is a
    // real answer here: it means "no offset was a pose often enough to name", and the
    // report says which bar.
    uint32_t bestOffset() const;
    // The same over the loose class: held in enough assemblies *and* seen to move. This is the
    // one that can tell a scaled pose from a basis matrix, where the strict bar says nothing
    // for both.
    uint32_t bestAffineOffset() const;
    // How many distinct identities one offset's global-or-per-object question is compared over.
    // Sixteen is enough to tell a global from a per-object value and small enough that the storage
    // is a rounding error on a display thread; the report carries the count and whether it was
    // capped, so a reader knows which of the two it is looking at.
    static constexpr size_t kIdentitySamples = 16;

    // The share of assemblies an offset must hold a transform in to be named.
    static constexpr uint32_t kBeliefPercent = 20;

  private:
    // The identity of a draw's object: its block sources, as the fork hands them over.
    //
    // **Measured not to recur, and that is a finding, not a detail.** Over 836,990 assembled
    // buffers this matched exactly one identity across 438,872 that had sources: the uniform
    // block is re-uploaded at a new guest address each frame, so the set of addresses is
    // nearly unique per draw and the same object's assemblies never meet. The comparison the
    // whole locator rests on -- "did this value change between two draws of one object" --
    // therefore happens a handful of times, not hundreds of thousands.
    //
    // The identity the objective names is the node, and the node is not in this record: the
    // binder sees it and the assembly hook does not. Correlating the two is the fix, and until
    // it is done the movement counts here are reported with the denominator beside them rather
    // than left to be divided from the assembly count.
    static std::string identityOf(const frame::RecordedUniformAssembly& assembly);
    // The last transform seen at an offset for one identity, so the next can be compared
    // with it. Bounded, and the refusal is counted. The offset is in BYTES, the unit the
    // candidates and the report both use: it was once a float index here and a byte
    // offset there, so no two readings ever matched and every comparison stayed at zero.
    bool remember(const std::string& identity, uint32_t byteOffset, const float* words);

    std::atomic<uint64_t> m_assemblies{0};
    mutable std::mutex m_mutex;
    // offset / 4 -> its counts. A buffer is a float array, so an offset in floats is the
    // unit the scan and the substitution both want.
    std::vector<Candidate> m_candidates;
    std::vector<std::pair<std::string, std::pair<uint32_t, std::array<float, kPoseWords>>>> m_seen;
    uint64_t m_unscanned = 0;
    uint64_t m_noSources = 0;
    // Whether any assembly carried a published object, so the report can say which of the two
    // keys the numbers came from. A run where every assembly fell back is a run whose
    // identities are addresses, and that is a different measurement from one whose are nodes.
    bool m_sawObjectAddress = false;
    uint64_t m_identitiesRefused = 0;
};

} // namespace wiiuport::title
