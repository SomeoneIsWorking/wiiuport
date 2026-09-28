#pragma once

#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/title/TransformShape.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
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
    // How many distinct alignments one matrix's offsets are kept for. A matrix in a flat run is
    // affine at up to twelve of them, so twelve is the whole of it and the bound is never the
    // thing that decides -- but it is a bound, and the report says when it was reached.
    static constexpr size_t kMaxAlignments = 12;
    // How many distinct (identity, candidate) pairs are remembered. A candidate is a matrix and a
    // matrix is seen at up to `kMaxAlignments` offsets, so the pairs are fewer than the old
    // per-offset pairs were and the bound is unchanged on purpose: it was never the thing that
    // decided, and `m_identitiesRefused` counts what it turned away.
    // How many distinct block-source identities are followed, so "does it move" is asked
    // of a bounded set of objects rather than of every draw the title has ever made.
    static constexpr size_t kIdentities = 16;

    // A candidate offset, and what was seen there.
    struct Candidate {
        // **The shader whose layout this candidate was found in.** An assembly is one shader's
        // uniform buffer, and a uniform block's layout is fixed: two draws of one shader put the
        // same uniform at the same slot every frame, and two *different* shaders may put the same
        // uniform at different slots. So the pose's offset in an assembly is a function of the
        // shader, not of the draw -- and the table was keyed on the offset alone, which pooled
        // every shader's layout together and read one matrix as eight candidates. That is the same
        // class of fault as pooling offsets over identities: the instrument was keyed on the wrong
        // subject, and no corpus finds a per-shader answer from a table that has thrown the shader
        // away.
        uint64_t shaderBaseHash = 0;
        uint64_t shaderAuxHash = 0;
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
        // **The alignments this matrix was seen at, and how many were observed.**
        //
        // A matrix written as a flat run of floats satisfies the class at every 4-byte alignment
        // inside it, so one matrix produces up to twelve candidates. Measured on the real title,
        // eight of this class's twelve held offsets were one array seen at eight alignments: laid
        // over each other their words agreed at every shared position and there was no
        // contradiction in seventeen words. **So a candidate is a matrix, not an alignment, and an
        // assembly counts once for it however many of its alignments were affine.** Without this,
        // `bestOffset` was the maximum over a run of offsets that are the same matrix, and it moved
        // between runs (60 in one, 104 in the next) for a reason that had nothing to do with the
        // title.
        //
        // **And the alignments are the fact the blend turns on: a pose's offset in an assembly is
        // not a constant, so no fixed offset can be written.** The report carries how many were
        // seen and whether the list was capped.
        std::vector<uint32_t> alignments;
        bool alignmentsCapped = false;
        // The alignments that came in this assembly but were folded into this candidate rather than
        // counted as a candidate of their own. Every folded alignment is a window on this matrix,
        // so this is the over-count the collapse removed, with its size beside it.
        uint64_t folded = 0;

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

    // **The share of a shader's own assemblies a candidate must hold a transform in to be
    // named.** It used to be a share of *every* assembly the run saw, which was a whole-frame
    // denominator compared against a per-shader count: with the table keyed on (shader, offset),
    // one of 298 candidates cleared it, and the one that did held a value that is not a matrix of
    // consequence. A per-shader count cannot reach a whole-frame bar, so the bar was a property of
    // the window's size rather than of the data. **A per-shader count is compared against its own
    // shader's assemblies, and the report carries that count**, so a reader can see which
    // denominator cleared the bar.
    //
    // **This is not mutation-verified, and the reason is recorded rather than papered over.** A
    // test was written for it and removed: it asserted the share each shader's entry reports, and a
    // candidate appears in the report's rigid table as well as its affine one, so the presence of
    // a shader in the body is not evidence it is in the table under test. Fixing that needs a
    // structured parse of the report rather than a substring search, and a test that finds the
    // *other* table's copy is worse than no test -- it reads as coverage and is not. The bar's
    // correctness rests on the argument above and on the run that found the fault, and the next
    // change to this class should bring a parser with it.
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
    // **Keyed on the shader as well as the offset**, for the reason the candidate table is: a
    // candidate is one shader's layout, and two shaders may hold a matrix at the same offset. A
    // lookup by offset alone finds whichever came first, and counts a second shader's assemblies
    // against the first shader's candidate.
    // The two the belief bar is measured against, both per shader. **The mutex is already held** by
    // every caller: they are called from inside `json()` and the two `best*` methods, and each of
    // those takes the lock once.
    uint64_t shaderAssembliesLocked(const Candidate& candidate) const;
    // A candidate that holds a transform in none of its own shader's assemblies has a
    // denominator of zero and a share of zero, which is the honest answer: the shader was seen
    // once and the candidate was not in it. Reported rather than skipped, because a candidate that
    // fails a bar for a reason the report does not name reads as a candidate that is not there.

    uint64_t shareOfShaderLocked(const Candidate& candidate, uint64_t inClass) const;
    bool clearsBarLocked(const Candidate& candidate, uint64_t inClass) const;

    bool remember(const std::string& identity, uint64_t shaderBaseHash, uint64_t shaderAuxHash,
                  uint32_t byteOffset, const float* words);

    std::atomic<uint64_t> m_assemblies{0};
    // How many assemblies each shader contributed, which is the denominator the belief bar is
    // measured against. Bounded by the number of distinct shaders a title can have, and the
    // overflow counted rather than dropped: a shader whose count is missing makes every candidate
    // of it fail the bar, and a candidate that fails a bar for a reason the report does not name
    // reads as a candidate that is not there.
    std::unordered_map<uint64_t, uint64_t> m_byShader;
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
