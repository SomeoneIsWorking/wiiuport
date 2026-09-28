#include "wiiuport/title/PoseBlend.h"

#include "wiiuport/interp/Blendable.h"
#include "wiiuport/title/JsonBody.h"
#include "wiiuport/title/PoseByShader.h"
#include "wiiuport/title/WindWakerPaint.h"

namespace wiiuport::title {

namespace {

// **The midpoint's weight, named.** It is 0.5 because the two paints are half a tick apart, so the
// in-between frame is the exact middle of the two poses and not a fraction chosen for latency: a
// weight other than a half would present a frame the game never was in, and the objective asks for
// the lerp of N-1 to N rather than for a plausible in-between.
constexpr float kHalf = 0.5f;

} // namespace

PoseBlend::Key PoseBlend::keyFor(uint32_t objectAddress, uint64_t shaderBaseHash) const {
    // **The node, where the title's own binder has said which node it is.** The fallback is the
    // shader, and it is a weak identity by a measurement rather than by an assumption: the block
    // sources matched exactly one identity across 438,872 that had them, because the uniform block
    // is re-uploaded at a new guest address each frame. So a pair held under the fallback is held
    // per shader rather than per object, and the count of those is reported beside the total,
    // because "held 590" means two different things under the two identities.
    const uint64_t identity = objectAddress != 0 ? 0x100000000ull | objectAddress : shaderBaseHash;
    return {identity, {objectAddress, shaderBaseHash}};
}

void PoseBlend::onAssemblyAtOffset(float* words, size_t count, uint32_t byteOffset,
                                   uint64_t shaderBaseHash, uint32_t objectAddress,
                                   bool inBetween) {
    if (words == nullptr) {
        return;
    }
    const size_t offset = byteOffset / sizeof(float);
    {
        std::scoped_lock lock(m_mutex);
        ++m_tally.assemblies;
        // **The range is checked against this assembly's own length.** A candidate found in a
        // 768-byte buffer can be handed a shorter one for a different draw of the same shader, and
        // a write past the end of a shorter buffer is a write into whatever is next -- which is how
        // a blend corrupts a value that is not its own.
        if (offset + kWords > count) {
            ++m_tally.outOfRange;
            return;
        }
    }

    std::scoped_lock lock(m_mutex);

    // **The title's own twelve words, read before any write and held whatever this paint is.** A
    // lerp used to return before the held copy was refreshed, so the held pose froze at the value
    // it was first given and every later lerp was measured from that: a midpoint between tick 0 and
    // tick N rather than between N-1 and N. That is a wrong place which looks right -- the picture
    // still moves, at the wrong speed -- and no counter would have said a fault.
    float* const at = words + offset;
    std::array<float, kWords> mine{};
    for (size_t word = 0; word < kWords; word++) {
        mine[word] = at[word];
    }

    const Key key = keyFor(objectAddress, shaderBaseHash);
    const auto held = m_held.find(key);
    const bool known = held != m_held.end();

    if (known && inBetween) {
        ++m_tally.inBetweenKnown;
        // **Every word must be one a lerp may touch, or nothing is written.** A partial pose is a
        // matrix with a row from one frame and a row from another, which is not a pose the game was
        // ever in; refusing the whole write leaves the tick's own value in place, which is at least
        // a frame the title drew.
        bool blendable = true;
        for (size_t word = 0; word < kWords && blendable; word++) {
            blendable = interp::isNumber(held->second[word]) && interp::isNumber(mine[word]);
        }
        if (!blendable) {
            ++m_tally.refusedUnblendable;
        } else {
            for (size_t word = 0; word < kWords; word++) {
                at[word] = held->second[word] + kHalf * (mine[word] - held->second[word]);
            }
            ++m_tally.lerped;
            m_tally.wordsWritten += kWords;
        }
    } else if (known) {
        ++m_tally.notInBetween;
    } else {
        ++m_tally.firstSight;
    }

    // **Held after the write decision, from the copy taken before it.** The bound is checked here,
    // at the point the pair would be added, and a refused pair leaves the previous one in place
    // rather than replacing it with a pose that is not the object's.
    if (!known) {
        if (m_held.size() >= kMaxHeld) {
            ++m_tally.refusedForRoom;
            return;
        }
        if (objectAddress == 0) {
            ++m_fallbackHeld;
        }
    }
    m_held[key] = mine;
    ++m_tally.refreshed;
}

void PoseBlend::onAssembly(float* words, size_t count, uint64_t shaderBaseHash,
                           uint64_t shaderAuxHash, uint32_t objectAddress, bool inBetween) {
    if (words == nullptr) {
        return;
    }
    std::string refusal;
    const auto offset = m_poses == nullptr
                            ? std::nullopt
                            : m_poses->offsetFor(shaderBaseHash, shaderAuxHash, refusal);
    if (!offset.has_value()) {
        std::scoped_lock lock(m_mutex);
        ++m_tally.assemblies;
        ++m_tally.withoutShader;
        return;
    }
    onAssemblyAtOffset(words, count, *offset, shaderBaseHash, objectAddress, inBetween);
}

void PoseBlend::onAssemblyBeforeDraw(float* words, size_t count, uint64_t shaderBaseHash,
                                     uint64_t shaderAuxHash, uint32_t node) {
    // **With no paint to ask, nothing is written.** A blend that cannot tell which paint of the
    // pair is in progress would be writing a midpoint into the tick's own frame, which is the one
    // place the title's own value must survive -- so the absence of the answer is a refusal and not
    // a default of "blend it anyway".
    const bool inBetween = m_paint != nullptr && m_paint->inBetweenPaint();
    onAssembly(words, count, shaderBaseHash, shaderAuxHash, node, inBetween);
}

uint64_t PoseBlend::held() const {
    std::scoped_lock lock(m_mutex);
    return static_cast<uint64_t>(m_held.size());
}

uint64_t PoseBlend::heldOnFallbackIdentity() const {
    std::scoped_lock lock(m_mutex);
    return m_fallbackHeld;
}

void PoseBlend::writeTo(JsonBody& body) const {
    body.number("assemblies", m_tally.assemblies);
    body.number("withoutShader", m_tally.withoutShader);
    body.number("outOfRange", m_tally.outOfRange);
    // The pairs, from the map, and the refreshes, from the counter: two numbers, two meanings, and
    // neither under the other's name.
    body.number("held", static_cast<uint64_t>(m_held.size()));
    body.number("refreshed", m_tally.refreshed);
    body.number("heldOnFallbackIdentity", m_fallbackHeld);
    body.number("refusedForRoom", m_tally.refusedForRoom);
    body.number("lerped", m_tally.lerped);
    body.number("inBetweenKnown", m_tally.inBetweenKnown);
    body.number("notInBetween", m_tally.notInBetween);
    body.number("firstSight", m_tally.firstSight);
    body.number("refusedUnblendable", m_tally.refusedUnblendable);
    body.number("wordsWritten", m_tally.wordsWritten);
    // The bound beside the count, so a full table is distinguishable from a short one.
    body.number("maxHeld", kMaxHeld);
    // **Of the in-between draws that could be blended, how many were.** The denominator is
    // `inBetweenKnown` -- the in-between paints over a pair that was already held -- and not the
    // assembly count, because a number divided by the frame's draw count says how busy the frame
    // was rather than whether the blend is happening. A table full of held poses and no lerps is
    // the exact shape of a blend that looks installed, and this is the field that says it is not.
    body.number("lerpsPerInBetween",
                m_tally.inBetweenKnown == 0 ? 0 : m_tally.lerped * 100 / m_tally.inBetweenKnown);
}

std::string PoseBlend::json() const {
    JsonBody body;
    writeTo(body);
    return body.finish();
}

} // namespace wiiuport::title
