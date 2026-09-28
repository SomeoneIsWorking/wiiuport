#include "wiiuport/title/PoseByShader.h"

#include "wiiuport/interp/Blendable.h"
#include "wiiuport/title/JsonBody.h"

#include <cmath>

namespace wiiuport::title {

void PoseByShader::feed(const Offered* candidates, size_t count) {
    m_lastFeed = Feed{};
    for (size_t index = 0; index < count; index++) {
        const Offered& one = candidates[index];
        Entry entry;
        entry.byteOffset = one.byteOffset;
        entry.otherObjects = one.otherObjects;
        entry.otherObjectsSame = one.otherObjectsSame;
        entry.moved = one.moved;
        entry.compared = one.compared;
        std::string refusal;
        offer(one.shaderBaseHash, one.shaderAuxHash, entry, refusal);
        ++m_lastFeed.offered;
        if (refusal.empty()) {
            ++m_lastFeed.accepted;
        } else {
            ++m_lastFeed.refused;
            if (m_lastFeed.firstRefusal.empty()) {
                m_lastFeed.firstRefusal = refusal;
            }
        }
    }
}

std::string PoseByShader::refused(const Entry& entry) const {
    // **The counts beside the reason, in every case.** "refused" with no numbers is a refusal a
    // caller cannot act on, and a blend that draws an object at N for a frame wants to say how many
    // it drew there and why.
    if (entry.compared == 0) {
        return "compared 0 assemblies, so nothing said whether it moves";
    }
    if (entry.moved == 0) {
        return "held the same twelve words in all " + std::to_string(entry.compared) +
               " of its shader's assemblies, so it is a basis matrix and not a pose";
    }
    if (entry.otherObjects == 0) {
        return "no other object was compared against it, so whether it belongs to one is unknown";
    }
    if (entry.otherObjectsSame >= kShareBar) {
        return "the same twelve words as " + std::to_string(entry.otherObjectsSame) + " of " +
               std::to_string(entry.otherObjects) +
               " other objects, so it is a pass's value and not an object's";
    }
    return "";
}

void PoseByShader::offer(uint64_t shaderBaseHash, uint64_t shaderAuxHash, const Entry& entry,
                         std::string& refusal) {
    ++m_tally.offered;
    refusal = refused(entry);
    if (!refusal.empty()) {
        if (entry.compared != 0 && entry.moved == 0) {
            ++m_tally.refusedStill;
        } else if (entry.otherObjects == 0) {
            ++m_tally.refusedNoOthers;
        } else {
            ++m_tally.refusedShared;
        }
        return;
    }
    ++m_tally.accepted;
    // **A later offer for a shader the table already has is refused, not merged.** Two candidates
    // for one shader are two different offsets, and picking one of them is a choice the evidence
    // does not make; a caller that has two is a caller that has a bug upstream, and the count says
    // so.
    const auto held = m_byShader.find({shaderBaseHash, shaderAuxHash});
    if (held != m_byShader.end()) {
        ++m_tally.refusedShared;
        refusal = "shader " + JsonBody::hex(shaderBaseHash) + " already has an offset, " +
                  std::to_string(held->second.byteOffset) + ", and this one says " +
                  std::to_string(entry.byteOffset);
        return;
    }
    m_byShader[{shaderBaseHash, shaderAuxHash}] = entry;
}

std::optional<uint32_t> PoseByShader::offsetFor(uint64_t shaderBaseHash, uint64_t shaderAuxHash,
                                                std::string& refusal) {
    ++m_tally.lookedUp;
    const auto held = m_byShader.find({shaderBaseHash, shaderAuxHash});
    if (held == m_byShader.end()) {
        refusal = "no offset for shader " + JsonBody::hex(shaderBaseHash) + " aux " +
                  JsonBody::hex(shaderAuxHash) +
                  ": a draw of it is placed from the title's own values and left alone";
        return std::nullopt;
    }
    ++m_tally.found;
    refusal.clear();
    return held->second.byteOffset;
}

uint32_t PoseByShader::blendableWords(const float* words) {
    // **Zero or a finite normal float, and the test is `TransformShape`'s neighbour rather than a
    // new rule**: a small integer in a float's bits reads as a denormal, and averaging two of them
    // makes an integer neither frame wrote. `Blendable` already owns that predicate and this asks
    // it, because two copies of a rule that decides what a blend may touch is one more thing to
    // fall out of step.
    uint32_t blendable = 0;
    for (size_t word = 0; word < kWords; word++) {
        if (interp::isNumber(words[word])) {
            ++blendable;
        }
    }
    return blendable;
}

void PoseByShader::writeTo(JsonBody& body) const {
    body.number("shareBar", kShareBar);
    body.number("poseWords", kWords);
    body.number("shaders", static_cast<uint64_t>(m_byShader.size()));
    body.number("offered", m_tally.offered);
    body.number("accepted", m_tally.accepted);
    body.number("refusedShared", m_tally.refusedShared);
    body.number("refusedStill", m_tally.refusedStill);
    body.number("refusedNoOthers", m_tally.refusedNoOthers);
    body.number("lookedUp", m_tally.lookedUp);
    body.number("found", m_tally.found);
    // The last feed's own counts, beside the running totals, so a reader sees both what one call
    // did and what the table holds -- and the first refusal, because a caller that armed this needs
    // to know why its candidate was turned away without asking again.
    body.number("offeredLastFeed", m_lastFeed.offered);
    body.number("acceptedLastFeed", m_lastFeed.accepted);
    body.number("refusedLastFeed", m_lastFeed.refused);
    body.string("firstRefusal", m_lastFeed.firstRefusal);
    JsonBody shaders;
    size_t index = 0;
    for (const auto& [shader, entry] : m_byShader) {
        JsonBody one;
        one.string("shader", JsonBody::hex(shader.first));
        one.string("shaderAux", JsonBody::hex(shader.second));
        one.number("byteOffset", entry.byteOffset);
        one.number("wordOffset", entry.byteOffset / 4u);
        one.number("otherObjectsSame", entry.otherObjectsSame);
        one.number("otherObjects", entry.otherObjects);
        one.number("moved", entry.moved);
        one.number("compared", entry.compared);
        shaders.object(std::to_string(index), one.text());
        ++index;
    }
    body.object("shaders", shaders.text());
}

std::string PoseByShader::json() const {
    JsonBody body;
    writeTo(body);
    return body.finish();
}

} // namespace wiiuport::title
