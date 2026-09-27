#pragma once

#include "wiiuport/frame/RecordingObserver.h"

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
    // The tolerances, as in the pose history: loose enough for a matrix built in single
    // precision through a chain of transforms, and stated so a number near the edge is a
    // number somebody can argue with.
    static constexpr float kUnitTolerance = 0.01f;
    static constexpr float kPerpendicularTolerance = 0.01f;
    static constexpr size_t kPoseWords = 12;
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
        uint64_t moved = 0;        // of those, how many found the value changed
        float biggestDelta = 0.0f; // the largest single-float change seen
        uint64_t identities = 0;   // distinct block-source sets seen holding one here
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
    // The share of assemblies an offset must hold a transform in to be named.
    static constexpr uint32_t kBeliefPercent = 20;

  private:
    // A rigid transform, tested.
    static bool isPose(const float* words);
    // The identity of a draw's object: its block sources, as the fork hands them over.
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
    uint64_t m_identitiesRefused = 0;
};

} // namespace wiiuport::title
