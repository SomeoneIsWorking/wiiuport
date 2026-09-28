#include "wiiuport/title/ObjectPoseLocator.h"
#include "wiiuport/title/TransformShape.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>

namespace wiiuport::title {

namespace {} // namespace

// Three unit rows, mutually perpendicular. Unit length and perpendicularity to a stated
// tolerance is what makes twelve floats a transform; a colour triple can be near unit
// length by chance and a matrix of ones has equal rows, so neither passes.

// The block sources as a string, so an identity is comparable without a comparator and a
// report can name one. The sources are the guest addresses the draw read its uniforms
// from, which is what survives a tick; the draw's place in the frame does not.
std::string ObjectPoseLocator::identityOf(const frame::RecordedUniformAssembly& assembly) {
    // **The node, when the title's own code has said which one.** `blockSources` is the
    // fallback and not the identity: measured over 836,990 assembled buffers it matched exactly
    // one identity across the 438,872 that had sources, because the uniform block is
    // re-uploaded at a new guest address each frame. A movement count over the one comparison
    // that produced is not a measurement, which is the whole reason the node is here.
    if (assembly.objectAddress != 0) {
        return "node:" + std::to_string(assembly.objectAddress);
    }
    return "blocks:" + std::to_string(assembly.blockSources[0]) + "," +
           std::to_string(assembly.blockSources.size());
}

// Compare this reading with the last one for the same identity at the same offset, and
// remember it. A transform that never moves is a colour triple that looked like one, so
// the movement count is what separates them -- and it is reported beside the count of
// comparisons, because a moving count with no denominator says nothing.
bool ObjectPoseLocator::remember(const std::string& identity, uint64_t shaderBaseHash,
                                 uint64_t shaderAuxHash, uint32_t offset, const float* words) {
    for (auto& entry : m_seen) {
        if (entry.second.first != offset || entry.first != identity) {
            continue;
        }
        auto known = std::find_if(m_candidates.begin(), m_candidates.end(),
                                  [offset, shaderBaseHash, shaderAuxHash](const Candidate& one) {
                                      return one.offset == offset &&
                                             one.shaderBaseHash == shaderBaseHash &&
                                             one.shaderAuxHash == shaderAuxHash;
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
    if (assembly.objectAddress != 0) {
        m_sawObjectAddress = true;
    }
    const std::string identity = identityOf(assembly);
    {
        // **The per-shader denominator, counted where the assemblies are counted.** It is the
        // number the belief bar is measured against, and without it a per-shader candidate is
        // compared against the whole frame -- which no single shader's layout can reach.
        std::scoped_lock lock(m_mutex);
        ++m_byShader[assembly.shaderBaseHash];
    }

    // Every 4-aligned offset, tested. A vector of candidates rather than a fixed-size
    // array, because a buffer's own length is what bounds the offsets and a fixed array
    // would either cap the scan below that or be sized for a guess.
    // Every 4-aligned offset, tested, and **collapsed before anything is counted.** A matrix
    // written as a flat run of floats is affine at every alignment inside it, so this loop used to
    // find one matrix eight times and each finding became a candidate with its own counts: eight
    // candidates for one value, and a best offset that was the maximum over a run of offsets that
    // are the same matrix. It moved between runs (60 in one, 104 in the next) for that reason
    // alone.
    //
    // A vector rather than a fixed-size array, because a buffer's own length is what bounds the
    // offsets and a fixed array would either cap the scan below that or be sized for a guess. Each
    // entry is an offset this assembly saw a *matrix* at, and the ones folded away are reported
    // beside the candidate they folded into.
    std::vector<uint32_t> seenHere;
    size_t foldedHere = 0;
    for (size_t offset = 0; offset + kPoseWords <= words; offset++) {
        if (!Shape::isAffine(assembly.data.data() + offset)) {
            continue;
        }
        const uint32_t at = static_cast<uint32_t>(offset * sizeof(float));
        if (Shape::sameShapeAs(seenHere, at)) {
            // A window on a matrix this assembly has already reported, not a matrix of its own.
            ++foldedHere;
            continue;
        }
        seenHere.push_back(at);
    }
    if (seenHere.empty()) {
        return;
    }

    std::scoped_lock lock(m_mutex);
    for (const uint32_t at : seenHere) {
        // **Keyed on the shader as well as the offset.** An assembly is one shader's uniform
        // buffer, and a uniform block's layout is fixed: two draws of one shader put the same
        // uniform at the same slot every frame, and two *different* shaders may put the same
        // uniform at different slots. The table was keyed on the offset alone, which pooled every
        // shader's layout together and read one matrix as eight candidates -- the same class of
        // fault as pooling offsets over identities, and for the same reason: the instrument was
        // keyed on the wrong subject.
        auto known = std::find_if(
            m_candidates.begin(), m_candidates.end(), [at, &assembly](const Candidate& one) {
                return one.offset == at && one.shaderBaseHash == assembly.shaderBaseHash &&
                       one.shaderAuxHash == assembly.shaderAuxHash;
            });
        if (known == m_candidates.end()) {
            Candidate candidate;
            candidate.shaderBaseHash = assembly.shaderBaseHash;
            candidate.shaderAuxHash = assembly.shaderAuxHash;
            candidate.offset = at;
            candidate.assemblies = 1;
            candidate.affineAssemblies = 1;
            if (Shape::isRigid(assembly.data.data() + at / sizeof(float))) {
                candidate.rigidAssemblies = 1;
            } else {
                candidate.biggestScale = Shape::scaleOf(assembly.data.data() + at / sizeof(float));
            }
            // `identities` is **derived** in `json()` from the pairs this class remembers, not
            // counted here: it was `= 1` here and never touched again, so every run reported one
            // identity at every offset and a reader sorting offsets by it sorted them by nothing.
            candidate.identities = 0;
            m_candidates.push_back(candidate);
            known = m_candidates.end() - 1;
        } else {
            known->assemblies++;
            known->affineAssemblies++;
            if (Shape::isRigid(assembly.data.data() + at / sizeof(float))) {
                known->rigidAssemblies++;
            } else {
                known->biggestScale = std::max(
                    known->biggestScale, Shape::scaleOf(assembly.data.data() + at / sizeof(float)));
            }
        }
        // The alignment this assembly saw the matrix at, and the over-count the collapse removed
        // beside it. **The alignments are the fact the blend turns on: the same matrix at two
        // offsets is two offsets to write, so a pose's offset in an assembly is not a constant.**
        if (std::find(known->alignments.begin(), known->alignments.end(), at) ==
            known->alignments.end()) {
            if (known->alignments.size() >= kMaxAlignments) {
                known->alignmentsCapped = true;
            } else {
                known->alignments.push_back(at);
            }
        }
        known->folded += foldedHere;
        // One unit throughout: the offset in bytes, as the candidates and the report both use
        // it, and the data indexed by it.
        remember(identity, assembly.shaderBaseHash, assembly.shaderAuxHash, at,
                 assembly.data.data() + at / sizeof(float));
    }

    // The global-or-per-object question, over the candidates as a whole rather than the ones this
    // assembly happened to produce: a candidate created by an earlier assembly is still a candidate
    // this assembly can say something about, and the best offset is very often one of those.
    //
    // **Inside the lock already held above, and that is not a style note.** This first took the
    // lock a second time -- the same non-recursive mutex, one scope apart -- and the test binary
    // hung rather than failing, which is the worst way for a lock mistake to announce itself: a
    // suite that stops reporting is a suite whose green is the last result anyone remembers.
    for (Candidate& candidate : m_candidates) {
        const size_t at = candidate.offset / sizeof(float);
        if (at + kPoseWords > words) {
            continue;
        }
        const float* value = assembly.data.data() + at;
        if (!candidate.hasLastValue) {
            candidate.identitiesSeen.push_back(identity);
            candidate.lastValue = std::array<float, kPoseWords>{};
            for (size_t word = 0; word < kPoseWords; word++) {
                candidate.lastValue[word] = value[word];
            }
            candidate.hasLastValue = true;
            continue;
        }
        // A repeat of an identity already compared is not a new identity, and counting it would
        // say a per-object pose differs from itself -- which is what it would do, every frame.
        if (candidate.identitiesSeen.size() >= kIdentitySamples) {
            candidate.identitySamplesCapped = true;
            continue;
        }
        if (std::find(candidate.identitiesSeen.begin(), candidate.identitiesSeen.end(), identity) !=
            candidate.identitiesSeen.end()) {
            continue;
        }
        // Compared against the assembly immediately before this one, which is the comparison the
        // question is actually about -- and then the retained value moves on, so the next object
        // is compared against *this* one. A value held by every object in a frame therefore reads
        // as shared however fast it changes, and a value each object owns reads as different on
        // every object after the first.
        bool same = true;
        for (size_t word = 0; word < kPoseWords; word++) {
            if (candidate.lastValue[word] != value[word]) {
                same = false;
                break;
            }
        }
        for (size_t word = 0; word < kPoseWords; word++) {
            candidate.lastValue[word] = value[word];
        }
        candidate.identitiesSeen.push_back(identity);
        candidate.otherIdentities++;
        if (same) {
            candidate.otherIdentitiesSame++;
        } else {
            candidate.otherIdentitiesDifferent++;
        }
    }
}

uint32_t ObjectPoseLocator::bestOffset() const {
    std::scoped_lock lock(m_mutex);
    uint32_t best = 0;
    uint64_t bestShare = 0;
    for (const Candidate& candidate : m_candidates) {
        if (!clearsBarLocked(candidate, candidate.rigidAssemblies)) {
            continue;
        }
        const uint64_t share = shareOfShaderLocked(candidate, candidate.rigidAssemblies);
        if (share > bestShare) {
            bestShare = share;
            best = candidate.offset;
        }
    }
    return best;
}

// The same over the loose class, which is the one that can tell a scaled transform from
// a basis matrix: the strict bar above says nothing for both.
uint32_t ObjectPoseLocator::bestAffineOffset() const {
    std::scoped_lock lock(m_mutex);
    uint32_t best = 0;
    uint64_t bestShare = 0;
    for (const Candidate& candidate : m_candidates) {
        if (candidate.moved == 0 || !clearsBarLocked(candidate, candidate.affineAssemblies)) {
            continue;
        }
        const uint64_t share = shareOfShaderLocked(candidate, candidate.affineAssemblies);
        if (share > bestShare) {
            bestShare = share;
            best = candidate.offset;
        }
    }
    return best;
}

// How many of one shader's own assemblies a candidate held a transform in, and whether that
// cleared the bar. **Both are per shader**, because a candidate is one shader's layout: a
// whole-frame denominator compared against a per-shader count is a property of the window rather
// than of the title, and it cleared one candidate of 298 in the run that found it.
uint64_t ObjectPoseLocator::shaderAssembliesLocked(const Candidate& candidate) const {
    const auto held = m_byShader.find(candidate.shaderBaseHash);
    return held == m_byShader.end() ? 0 : held->second;
}

uint64_t ObjectPoseLocator::shareOfShaderLocked(const Candidate& candidate,
                                                uint64_t inClass) const {
    const auto held = m_byShader.find(candidate.shaderBaseHash);
    if (held == m_byShader.end() || held->second == 0) {
        return 0;
    }
    return inClass * 100 / held->second;
}

bool ObjectPoseLocator::clearsBarLocked(const Candidate& candidate, uint64_t inClass) const {
    return shareOfShaderLocked(candidate, inClass) >= kBeliefPercent;
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
    // **Distinct identities, not tracked pairs.** `m_seen` holds one entry per (identity,
    // offset), so its size is not the number of objects -- a field named for identities and
    // carrying pairs is worse than no field, because it is the number a reader divides the
    // movement counts by. Both are reported, under their own names.
    std::unordered_set<std::string> distinct;
    for (const auto& entry : m_seen) {
        distinct.insert(entry.first);
    }
    const uint64_t identities = static_cast<uint64_t>(distinct.size());
    body.number("identitiesSeen", identities);
    body.number("trackedPairs", static_cast<uint64_t>(m_seen.size()));
    // Which key was in use, so a number is never silently from the weaker of two.
    body.string("identitySource", m_sawObjectAddress ? "objectAddress" : "blockSources");
    body.number("identitiesRefusedForTracking", m_identitiesRefused);
    // **How many distinct shaders the assemblies were, and how many distinct offsets the moving
    // matrices were at.** The pair is the measurement, and it is new: a candidate is one shader's
    // layout, so the same matrix lands at a different offset in each shader that reads it. An
    // offset that is a camera is therefore *one per shader*, and the ratio of the two counts says
    // which of those an offset is -- one count each is a single shader, equal counts is a value
    // every shader shares at its own offset, and more offsets than shaders is neither.
    //
    // It replaces a single "best offset" that was a maximum over offsets belonging to different
    // shaders, so it named whichever shader happened to place its matrix lowest and moved between
    // runs for that reason alone.
    // **The shaders *seen*, not the shaders that happened to produce a candidate.** These were
    // counted from `m_candidates`, so a shader whose layout held nothing in the class was missing
    // from the count -- and this is the number the belief bar divides by, so a shader missing from
    // it makes every candidate of that shader look like it was measured against too little.
    const uint64_t shaders = static_cast<uint64_t>(m_byShader.size());
    std::unordered_set<uint32_t> movingAt;
    for (const Candidate& candidate : m_candidates) {
        if (candidate.moved > 0) {
            movingAt.insert(candidate.offset);
        }
    }
    body.number("distinctShaders", shaders);
    body.number("distinctMovingOffsets", static_cast<uint64_t>(movingAt.size()));
    // Two bars over two classes, and the answer to where to look next is which of them
    // fired. `rigid` alone means the value is a pose; `affine` alone means it is a transform
    // carrying scale, which the strict bar would never have counted; neither means the pose
    // is not in the assembled buffers either. An offset is named only if it also cleared the
    // assemblies bar *and* was seen to move -- a value that holds a shape in every assembly
    // and never changes is a basis.
    // The bar, per shader, and the denominator that cleared it, per candidate. **The report
    // carries the count the share was taken of**, because a share without its denominator is a
    // number a reader cannot check and this report has named a best offset on a whole-frame
    // denominator before.
    auto clears = [this](const Candidate& candidate, uint64_t inClass) {
        return clearsBarLocked(candidate, inClass);
    };
    // Every offset that cleared the assemblies bar is listed, and each says whether it moved.
    // Only the moving ones are named. Filtering the immovable ones out of the table leaves the
    // most informative case -- a shape held in every assembly that never changes -- reported
    // as an empty list, which is indistinguishable from a scan that found nothing at all. That
    // emptiness is what the first version of this did, and it is the case a reader most needs
    // the counts for.
    auto tableFor = [this, &clears](bool affine) {
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
            if (clears(candidate, inClass(candidate))) {
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
        // **The denominator is per shader now, so a single number for it would be a
        // contradiction.** What is reported is the share the bar was applied to and the count it
        // was taken of, per entry, and the bar itself -- and a reader who wants the whole frame
        // has `assemblies` above to divide by.
        table.raw("beliefPercent", std::to_string(kBeliefPercent));
        table.raw("bestOffset", best);
        JsonBody entries;
        for (size_t index = 0; index < held.size() && index < kExamples; index++) {
            const Candidate& candidate = held[index];
            JsonBody one;
            // **The shader first, then its own counts.** A reader looks an entry up by the shader
            // it belongs to, and the counts after it are that shader's -- so the shader is named
            // before them, and a search from the name forward finds this entry's own numbers
            // rather than the next entry's.
            //
            // **Two shaders may hold a matrix at the same offset**, and the count of distinct
            // shaders a moving matrix appears in is the measurement the blend turns on: a camera is
            // in every shader, a per-object pose in one.
            one.string("shaderBaseHash", JsonBody::hex(candidate.shaderBaseHash));
            one.string("shaderAuxHash", JsonBody::hex(candidate.shaderAuxHash));
            one.number("offset", candidate.offset);
            // The share of its own shader's assemblies, and the count the share was taken of.
            // Both, because the belief bar is per shader and a share without its denominator is a
            // number a reader cannot check.
            one.number("assembliesInShader", shaderAssembliesLocked(candidate));
            one.number("shareOfItsShader", shareOfShaderLocked(candidate, inClass(candidate)));
            one.number("assemblies", inClass(candidate));
            one.number("compared", candidate.compared);
            one.number("moved", candidate.moved);
            one.number("still", candidate.still);
            one.raw("moving", candidate.moved > 0 ? "true" : "false");
            one.raw("biggestDelta", JsonBody::real(static_cast<double>(candidate.biggestDelta)));
            one.raw("scale", JsonBody::real(static_cast<double>(candidate.biggestScale)));
            // **Derived, not counted.** `identities` is how many distinct objects have held this
            // offset, and the structure that knows is the per-(identity, offset) history -- so it
            // is read from there. A counter maintained beside it was `= 1` on the candidate's first
            // appearance and never moved, which is a report field that is a constant and reads like
            // a measurement. And this is a different number from `otherIdentities`, which is the
            // bounded sample the cross-object comparison used, and from `otherIdentitiesSameValue`,
            // which is what that comparison concluded.
            uint64_t holders = 0;
            for (const auto& [heldIdentity, held] : m_seen) {
                if (held.first == candidate.offset) {
                    ++holders;
                }
            }
            one.number("identities", holders);
            // **The collapse, with both halves.** `alignmentsSeen` is how many places in the
            // frame's assemblies this one matrix was found, `folded` is how many candidate offsets
            // it would have been without the collapse, and `candidatesBefore` is what the class
            // counted before it was folded. All three, because a count whose reader cannot see what
            // it removed cannot be compared with a count from a class that has not folded.
            one.number("alignmentsSeen", static_cast<uint64_t>(candidate.alignments.size()));
            one.raw("alignmentsCapped", candidate.alignmentsCapped ? "true" : "false");
            one.number("foldedFromAlignments", candidate.folded);
            std::string at = "[";
            for (size_t index = 0; index < candidate.alignments.size(); index++) {
                at += (index == 0 ? "" : ",") + std::to_string(candidate.alignments[index]);
            }
            at += "]";
            one.raw("alignments", at);
            // **The value itself, as this class last read it.** The counts say how often an offset
            // held a transform and how much of the frame shared it; they do not say *which* matrix
            // it was. The title names its own camera -- `cWorldViewMatrix[0]` at 0x10163bb4 -- so
            // with the words here a camera becomes a comparison against a named address rather than
            // an inference from a movement pattern, and a value that matches nothing can be seen to
            // match nothing.
            //
            // The last value this class saw, which for a per-tick matrix is the one from the most
            // recent assembly at that offset -- not a tick N-1 value, and not averaged.
            // `JsonBody::raw` names every value it writes, so an array is built here as the one
            // string it is: a body whose elements are named is an object, and the reader would then
            // be handed a map where a sequence is meant.
            std::string words = "[";
            for (size_t word = 0; word < kPoseWords; word++) {
                words += (word == 0 ? "" : ",") +
                         JsonBody::real(static_cast<double>(candidate.lastValue[word]));
            }
            words += "]";
            one.raw("lastValue", words);
            // The global-or-per-object question, three numbers that answer it between them: a
            // candidate compared against two or more objects and never differing from the first is
            // a value every object shares, which is what a camera's view matrix is. Reported for
            // every offset, because an offset that is per-object and one that is global are both
            // answers and only one of them is what a blend wants.
            one.number("otherIdentities", candidate.otherIdentities);
            one.number("otherIdentitiesSameValue", candidate.otherIdentitiesSame);
            one.number("otherIdentitiesDifferentValue", candidate.otherIdentitiesDifferent);
            one.string("identitySamplesCapped", candidate.identitySamplesCapped ? "yes" : "no");
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
