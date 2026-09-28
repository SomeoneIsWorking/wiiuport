#pragma once

#include "wiiuport/title/JsonBody.h"

#include "wiiuport/frame/RecordingObserver.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wiiuport::title {

class PoseByShader;
class WindWakerPaint;

// **The in-between frame, written at the title's own draw.** One unit, one job: when a draw's
// uniform buffer arrives with the pose at a known offset, and this is the in-between paint, write
// the midpoint between this node's pose at tick N-1 and its pose at tick N into those twelve words,
// and let the title's own draw produce everything else -- the skinning, the attributes, the display
// list, the flip.
//
// **Why the write is here and nowhere else.** The assembled buffer is the runtime's own memory and
// the title reads its own uniforms from it, so a write needs no native override, no patch to the
// title's code, and nothing on the player's disc image. It happens after the title has assembled
// the buffer for this draw and before the draw is issued, which is the only window in which the
// value the title is about to transform with is the value being written.
//
// **Which two values, and in which order.** The held value is the pose this node carried at the
// *previous* tick, and the value in the buffer is the pose it carries at *this* one, so the
// midpoint is of tick N-1 to tick N. The held copy is taken **before** the write, so the pose
// stored is always the title's own and never a lerp of one -- which is the failure that would make
// a second tick's lerp a third of the way from the wrong place.
//
// **And the order the two paints are presented in.** The objective puts the lerp of tick N-1 to
// tick N *before* the tick's own frame, so within one tick's two paints this one is the first and
// the tick's own is the second. `WindWakerPaint::inBetweenPaint()` says which paint is in progress;
// the flag arrives here rather than being derived from the paint count in a second place, because a
// parity computed twice is two rules about the same thing.

class PoseBlend final : public frame::AssemblyBeforeDrawListener {
  public:
    // How many (node, shader) pairs are held. One node's pose has to survive from the tick that
    // drew it to the tick that lerps it, so this is a frame's worth of objects rather than a draw
    // count; 4096 is far above the 590 the title's binder measured, and the bound is reported when
    // it is reached rather than deciding anything quietly.
    static constexpr size_t kMaxHeld = 4096;

    // The float count of a pose: three rows of four.
    static constexpr size_t kWords = 12;

    struct Tally {
        uint64_t assemblies = 0;    // every assembly this unit was handed
        uint64_t withoutShader = 0; // the pose table had no offset for that draw's shader
        uint64_t outOfRange = 0;    // the offset is past the end of this assembly's buffer
        // **Assemblies whose pair was refreshed, not pairs held.** These were one field once, and
        // two meanings under one name: `held` counted every assembly that kept a pose up to date,
        // which grows with the frame's draw count, while the number of pairs being held does not.
        // A ratio divided by it was therefore lerps per *assembly*, and a frame with one object and
        // a thousand draws read as a thousand pairs. The pair count is the map's size, reported as
        // `held`; this counter is `refreshed` for what it counts.
        uint64_t refreshed = 0;
        uint64_t refusedForRoom = 0; // pairs it turned away because the bound was reached
        // In-between paints over a pair that was already held, whether or not the write happened.
        // **The denominator the blend's hit rate is taken of**, counted here because the
        // alternative -- dividing lerps by assemblies -- is a number about how busy the frame was.
        uint64_t inBetweenKnown = 0;
        uint64_t lerped = 0;             // of those, the ones written
        uint64_t notInBetween = 0;       // held, with nothing written: this is the tick's own paint
        uint64_t firstSight = 0;         // held for the first time, so there is no N-1 yet
        uint64_t refusedUnblendable = 0; // a word a lerp may not touch, so nothing was written
        uint64_t wordsWritten = 0;       // the float writes that got through
    };

    // The key is (node, shader), because the pose is per object and its offset is per shader, and
    // the two are different questions. **The node is the identity** where the title's own binder
    // has said which node it is; without that, the fallback is the block sources, which measured
    // nearly unique per draw, so a fallback pair is held and compared and its count is reported --
    // a reader can see whether the identity it is looking at came from the title or from a
    // fallback.
    using Key = std::pair<uint64_t, std::pair<uint32_t, uint64_t>>;

    PoseBlend() = default;

    // The table is held by reference and not owned: it is the measurement's, it is filled by
    // `POST /pose`, and a blend that owned a copy would be a blend reading its own numbers rather
    // than the ones the census gave.
    explicit PoseBlend(PoseByShader& poses) : m_poses(&poses) {
    }

    // One assembly, as the fork hands it over, with the pose resolved through the table this unit
    // was given. A draw whose shader the table has never seen is counted and left exactly as the
    // title wrote it.
    void onAssembly(float* words, size_t count, uint64_t shaderBaseHash, uint64_t shaderAuxHash,
                    uint32_t objectAddress, bool inBetween);

    // **The same, with the offset already resolved by the caller.** A separate *name* rather than
    // an overload: the two forms differ in whether the third argument is a byte offset or a shader
    // hash, and a call with a 32-bit literal matches the offset form exactly while the 64-bit hash
    // does not, so the compiler picks one without a word. That is not a theoretical hazard -- the
    // first version of this class had it, and a call that meant a shader was silently read as an
    // offset past the end of a buffer, which is a write into whatever is next.
    void onAssemblyAtOffset(float* words, size_t count, uint32_t byteOffset,
                            uint64_t shaderBaseHash, uint32_t objectAddress, bool inBetween);

    // **The observer's seam, implemented.** The flag comes from the paint module rather than being
    // counted here, because "which paint of the pair is this" is a fact about the stand-in and a
    // second parity computed in this class is a second rule about it.
    void onAssemblyBeforeDraw(float* words, size_t count, uint64_t shaderBaseHash,
                              uint64_t shaderAuxHash, uint32_t node) override;

    // The paint that says which of the two paints is in progress. Held by pointer and not owned: it
    // is the title's stand-in, and a blend that owned its own copy of the paint count would be
    // answering a question about a number that is not the title's.
    void setPaint(const WindWakerPaint* paint) {
        m_paint = paint;
    }

    Tally tally() const {
        std::scoped_lock lock(m_mutex);
        return m_tally;
    }

    // How many pairs are held, and how many of those came from a fallback identity rather than from
    // the title's own binder. **Reported beside the count**, because a blend keyed on a fallback is
    // a blend whose pairs are nearly unique per draw and whose "held" count is not a count of
    // objects.
    uint64_t held() const;
    uint64_t heldOnFallbackIdentity() const;

    // **The body as this class sees it**, so a caller that reports two owners in one document
    // composes it out of two `writeTo` calls rather than out of two rendered strings. Splicing
    // rendered documents is how a report grows a second one, and this project has paid for that
    // once; `json()` is `writeTo` and `finish()`, so there is one implementation of each field.
    void writeTo(JsonBody& body) const;
    std::string json() const;

  private:
    // The key a draw is filed under, and whether it came from the title or from the fallback.
    Key keyFor(uint32_t objectAddress, uint64_t shaderBaseHash) const;

    PoseByShader* m_poses = nullptr;
    const WindWakerPaint* m_paint = nullptr;
    mutable std::mutex m_mutex;
    std::map<Key, std::array<float, kWords>> m_held;
    Tally m_tally;
    uint64_t m_fallbackHeld = 0;
};

} // namespace wiiuport::title
