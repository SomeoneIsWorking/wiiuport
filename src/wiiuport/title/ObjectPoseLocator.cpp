#include "wiiuport/title/ObjectPoseLocator.h"
#include "wiiuport/title/TransformShape.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace wiiuport::title {

namespace {} // namespace

// Three unit rows, mutually perpendicular. Unit length and perpendicularity to a stated
// tolerance is what makes twelve floats a transform; a colour triple can be near unit
// length by chance and a matrix of ones has equal rows, so neither passes.

// The block sources as a string, so an identity is comparable without a comparator and a
// report can name one. The sources are the guest addresses the draw read its uniforms
// from, which is what survives a tick; the draw's place in the frame does not.
std::string ObjectPoseLocator::identityOf(const frame::RecordedUniformAssembly& assembly) {
    std::string out;
    char text[16];
    for (size_t index = 0; index < assembly.blockSources.size(); index++) {
        std::snprintf(text, sizeof(text), "%08x", assembly.blockSources[index]);
        out += text;
    }
    return out;
}

// Compare this reading with the last one for the same identity at the same offset, and
// remember it. A transform that never moves is a colour triple that looked like one, so
// the movement count is what separates them -- and it is reported beside the count of
// comparisons, because a moving count with no denominator says nothing.
bool ObjectPoseLocator::remember(const std::string& identity, uint32_t offset, const float* words) {
    for (auto& entry : m_seen) {
        if (entry.second.first != offset || entry.first != identity) {
            continue;
        }
        auto known =
            std::find_if(m_candidates.begin(), m_candidates.end(), [offset](const Candidate& one) {
                return one.offset == offset;
            });
        if (known == m_candidates.end()) {
            // The candidate is created by the caller before this, so it cannot happen --
            // and dereferencing the end of a vector to find out is how a cannot-happen
            // becomes undefined rather than merely untrue.
            return true;
        }
        known->compared++;
        // A magnitude, and a movement bar, not a bitwise test. The node locator reported
        // `moved 18` beside `biggest delta 0.000000` -- values differing in the last mantissa
        // bit -- and named three static matrices that way. The delta is recorded either way
        // here, because a delta that is only kept when it passes the bar cannot be looked at
        // to work out why it did not.
        const float biggest = Shape::deltaOf(entry.second.second.data(), words);
        if (Shape::moved(entry.second.second.data(), words)) {
            known->moved++;
        } else {
            known->still++;
        }
        known->biggestDelta = std::max(known->biggestDelta, biggest);
        for (size_t word = 0; word < kPoseWords; word++) {
            entry.second.second[word] = words[word];
        }
        return true;
    }
    if (m_seen.size() >= kIdentities * kExamples) {
        m_identitiesRefused++;
        return false;
    }
    std::array<float, kPoseWords> held{};
    for (size_t word = 0; word < kPoseWords; word++) {
        held[word] = words[word];
    }
    m_seen.emplace_back(identity, std::pair<uint32_t, std::array<float, kPoseWords>>(offset, held));
    return false;
}

void ObjectPoseLocator::onAssemblyRecorded(const frame::RecordedUniformAssembly& assembly) {
    m_assemblies.fetch_add(1, std::memory_order_relaxed);
    // A buffer larger than the scan's bound is reported as unscanned, not scanned in
    // part: a partial scan's "no pose here" is about the part it looked at.
    if (assembly.data.size() * sizeof(float) > kMaxScanBytes) {
        std::scoped_lock lock(m_mutex);
        m_unscanned++;
        return;
    }
    if (assembly.blockSources.empty()) {
        std::scoped_lock lock(m_mutex);
        m_noSources++;
        return;
    }
    const size_t words = assembly.data.size();
    if (words < kPoseWords) {
        return;
    }
    const std::string identity = identityOf(assembly);

    // Every 4-aligned offset, tested. A vector of candidates rather than a fixed-size
    // array, because a buffer's own length is what bounds the offsets and a fixed array
    // would either cap the scan below that or be sized for a guess.
    std::vector<Candidate> found;
    for (size_t offset = 0; offset + kPoseWords <= words; offset++) {
        if (!Shape::isAffine(assembly.data.data() + offset)) {
            continue;
        }
        Candidate candidate;
        candidate.offset = static_cast<uint32_t>(offset * sizeof(float));
        candidate.assemblies = 1;
        candidate.affineAssemblies = 1;
        if (Shape::isRigid(assembly.data.data() + offset)) {
            candidate.rigidAssemblies = 1;
        } else {
            candidate.biggestScale = Shape::scaleOf(assembly.data.data() + offset);
        }
        found.push_back(candidate);
    }
    if (found.empty()) {
        return;
    }

    std::scoped_lock lock(m_mutex);
    for (Candidate& candidate : found) {
        auto known = std::find_if(m_candidates.begin(), m_candidates.end(),
                                  [&candidate](const Candidate& one) {
                                      return one.offset == candidate.offset;
                                  });
        if (known == m_candidates.end()) {
            candidate.identities = 1;
            m_candidates.push_back(candidate);
            known = m_candidates.end() - 1;
        } else {
            known->assemblies++;
            known->affineAssemblies++;
            if (Shape::isRigid(assembly.data.data() + candidate.offset / sizeof(float))) {
                known->rigidAssemblies++;
            } else {
                known->biggestScale =
                    std::max(known->biggestScale, Shape::scaleOf(assembly.data.data() +
                                                                 candidate.offset / sizeof(float)));
            }
        }
        // One unit throughout: the offset in bytes, as the candidates and the report keep
        // it, and the data indexed by it.
        remember(identity, candidate.offset,
                 assembly.data.data() + candidate.offset / sizeof(float));
    }
}

uint32_t ObjectPoseLocator::bestOffset() const {
    std::scoped_lock lock(m_mutex);
    const uint64_t assemblies = m_assemblies.load();
    if (assemblies == 0) {
        return 0;
    }
    const uint64_t needed = assemblies * kBeliefPercent / 100;
    uint32_t best = 0;
    uint64_t bestAssemblies = 0;
    for (const Candidate& candidate : m_candidates) {
        if (candidate.rigidAssemblies < needed || candidate.moved == 0) {
            continue;
        }
        if (candidate.rigidAssemblies > bestAssemblies) {
            bestAssemblies = candidate.rigidAssemblies;
            best = candidate.offset;
        }
    }
    return best;
}

// The same over the loose class, which is the one that can tell a scaled transform from
// a basis matrix: the strict bar above says nothing for both.
uint32_t ObjectPoseLocator::bestAffineOffset() const {
    std::scoped_lock lock(m_mutex);
    const uint64_t assemblies = m_assemblies.load();
    if (assemblies == 0) {
        return 0;
    }
    const uint64_t needed = assemblies * kBeliefPercent / 100;
    uint32_t best = 0;
    uint64_t bestAssemblies = 0;
    for (const Candidate& candidate : m_candidates) {
        if (candidate.affineAssemblies < needed || candidate.moved == 0) {
            continue;
        }
        if (candidate.affineAssemblies > bestAssemblies) {
            bestAssemblies = candidate.affineAssemblies;
            best = candidate.offset;
        }
    }
    return best;
}

std::string ObjectPoseLocator::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    // The same names this report has always used. A field renamed because the code around
    // it moved is a field every existing reader silently loses, and a reader that gets no
    // value where a count used to be cannot tell a rename from a measurement that stopped
    // happening.
    const uint64_t assemblies = m_assemblies.load();
    body.number("assemblies", assemblies);
    body.number("beliefPercent", kBeliefPercent);
    body.number("maxScanBytes", kMaxScanBytes);
    body.number("unscannedBuffers", m_unscanned);
    body.number("buffersWithoutSources", m_noSources);
    body.number("identitiesRefused", m_identitiesRefused);
    // Every candidate offset the scan ever saw, in either class, so "no offset was named" is
    // a statement about a set and not an absence.
    body.number("candidates", m_candidates.size());
    body.raw("motionEpsilon", JsonBody::real(static_cast<double>(Shape::kMotionEpsilon)));
    body.raw("determinantFloor", JsonBody::real(static_cast<double>(Shape::kDeterminantFloor)));
    body.raw("scaleCeiling", JsonBody::real(static_cast<double>(Shape::kScaleCeiling)));
    // **The coverage the movement counts rest on, at the top of the report where it cannot
    // be read past.** Measured: 836,990 assemblies, 438,872 of them with block sources, and
    // exactly ONE identity ever matched a previous reading -- 63 repeat comparisons against
    // 262,978 assemblies at the offset it named. So the movement counts in this report are
    // counts over a handful of comparisons, not over the assemblies beside them, and a reader
    // who divides one by the other is dividing the wrong things.
    //
    // The cause is in the identity itself. `blockSources` is documented as "the engine's own
    // storage for the object, and the only identity a recorded draw carries", and the
    // measurement contradicts that: the uniform block is re-uploaded at a new address each
    // frame, so the set of addresses is nearly unique per draw and the same object's
    // assemblies never meet. The objective's answer is the node, and the node is not in this
    // record -- the binder sees it and the assembly hook does not. Until the two are
    // correlated, `movementHere` is a measurement over a very small denominator and the
    // belief bars below cannot be believed from it.
    body.number("identitiesSeen", static_cast<uint64_t>(m_seen.size()));
    body.number("identitiesRefusedForTracking", m_identitiesRefused);
    // Two bars over two classes, and the answer to where to look next is which of them
    // fired. `rigid` alone means the value is a pose; `affine` alone means it is a transform
    // carrying scale, which the strict bar would never have counted; neither means the pose
    // is not in the assembled buffers either. An offset is named only if it also cleared the
    // assemblies bar *and* was seen to move -- a value that holds a shape in every assembly
    // and never changes is a basis.
    const uint64_t needed = m_assemblies.load() * kBeliefPercent / 100;
    auto clears = [needed](uint64_t inClass) {
        return inClass >= needed;
    };
    // Every offset that cleared the assemblies bar is listed, and each says whether it moved.
    // Only the moving ones are named. Filtering the immovable ones out of the table leaves the
    // most informative case -- a shape held in every assembly that never changes -- reported
    // as an empty list, which is indistinguishable from a scan that found nothing at all. That
    // emptiness is what the first version of this did, and it is the case a reader most needs
    // the counts for.
    auto tableFor = [this, &clears, needed](bool affine) {
        auto inClass = [affine](const Candidate& candidate) {
            return affine ? candidate.affineAssemblies : candidate.rigidAssemblies;
        };
        JsonBody table;
        std::vector<Candidate> held;
        size_t ever = 0;
        for (const Candidate& candidate : m_candidates) {
            if (inClass(candidate) > 0) {
                ever++;
            }
            if (clears(inClass(candidate))) {
                held.push_back(candidate);
            }
        }
        std::sort(held.begin(), held.end(), [&](const Candidate& a, const Candidate& b) {
            const uint64_t left = inClass(a);
            const uint64_t right = inClass(b);
            if (left != right) {
                return left > right;
            }
            return a.offset < b.offset;
        });
        size_t moving = 0;
        std::string best = "null";
        for (const Candidate& candidate : held) {
            if (candidate.moved > 0) {
                moving++;
                if (best == "null") {
                    // An integer, because a byte offset is a byte offset: as a float it reads
                    // "64.0", and a caller that writes at it wants a number it can add.
                    best = std::to_string(candidate.offset);
                }
            }
        }
        // How many offsets were ever in this class at all, so "one offset was ever a rigid
        // transform" is a statement about the class rather than about the whole scan. The
        // loose class legitimately finds more, and a test asserting one candidate against the
        // loose total would be asserting that the loose class fails.
        table.number("candidates", ever);
        table.number("heldOften", held.size());
        table.number("believedOffsets", moving);
        table.raw("beliefPercent", std::to_string(kBeliefPercent));
        table.raw("assembliesNeeded", std::to_string(needed));
        table.raw("bestOffset", best);
        JsonBody entries;
        for (size_t index = 0; index < held.size() && index < kExamples; index++) {
            const Candidate& candidate = held[index];
            JsonBody one;
            one.number("offset", candidate.offset);
            one.number("assemblies", inClass(candidate));
            one.number("compared", candidate.compared);
            one.number("moved", candidate.moved);
            one.number("still", candidate.still);
            one.raw("moving", candidate.moved > 0 ? "true" : "false");
            one.raw("biggestDelta", JsonBody::real(static_cast<double>(candidate.biggestDelta)));
            one.raw("scale", JsonBody::real(static_cast<double>(candidate.biggestScale)));
            one.number("identities", candidate.identities);
            entries.object(std::to_string(index), one.text());
        }
        table.object("offsets", entries.text());
        return table.finish();
    };

    body.object("rigid", tableFor(false));
    body.object("affine", tableFor(true));
    return body.finish();
}

} // namespace wiiuport::title
