#include "wiiuport/title/ObjectPoseLocator.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace wiiuport::title {

namespace {

std::string number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.6f", static_cast<double>(value));
    return {text};
}

} // namespace

// Three unit rows, mutually perpendicular. Unit length and perpendicularity to a stated
// tolerance is what makes twelve floats a transform; a colour triple can be near unit
// length by chance and a matrix of ones has equal rows, so neither passes.
bool ObjectPoseLocator::isPose(const float* words) {
    for (size_t row = 0; row < 3; row++) {
        const float x = words[row * 3 + 0];
        const float y = words[row * 3 + 1];
        const float z = words[row * 3 + 2];
        if (std::fabs(std::sqrt(x * x + y * y + z * z) - 1.0f) > kUnitTolerance) {
            return false;
        }
    }
    for (size_t first = 0; first < 3; first++) {
        for (size_t second = first + 1; second < 3; second++) {
            float dot = 0.0f;
            for (size_t column = 0; column < 3; column++) {
                dot += words[first * 3 + column] * words[second * 3 + column];
            }
            if (std::fabs(dot) > kPerpendicularTolerance) {
                return false;
            }
        }
    }
    return true;
}

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
        float biggest = 0.0f;
        bool differing = false;
        for (size_t word = 0; word < kPoseWords; word++) {
            biggest = std::max(biggest, std::fabs(words[word] - entry.second.second[word]));
            if (words[word] != entry.second.second[word]) {
                differing = true;
            }
        }
        if (differing) {
            known->moved++;
            known->biggestDelta = std::max(known->biggestDelta, biggest);
        }
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
        if (!isPose(assembly.data.data() + offset)) {
            continue;
        }
        found.push_back(Candidate{static_cast<uint32_t>(offset * sizeof(float)), 1, 0, 0, 0.0f, 0});
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
    uint64_t bestSeen = 0;
    for (const Candidate& candidate : m_candidates) {
        if (candidate.assemblies < needed) {
            continue;
        }
        if (candidate.assemblies > bestSeen) {
            bestSeen = candidate.assemblies;
            best = candidate.offset;
        }
    }
    return best;
}

std::string ObjectPoseLocator::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    const uint64_t assemblies = m_assemblies.load();
    body.number("assemblies", assemblies);
    body.number("beliefPercent", kBeliefPercent);
    body.number("maxScanBytes", kMaxScanBytes);
    body.number("unscannedBuffers", m_unscanned);
    body.number("buffersWithoutSources", m_noSources);
    body.number("identitiesRefused", m_identitiesRefused);
    body.number("candidates", m_candidates.size());

    // The believed offset, with the count that made it believed, and the number of
    // distinct objects it was seen on -- a transform seen once, on one draw, is a
    // coincidence until it is seen on many.
    const uint64_t needed = assemblies * kBeliefPercent / 100;
    std::vector<Candidate> believed;
    for (const Candidate& candidate : m_candidates) {
        if (candidate.assemblies >= needed) {
            believed.push_back(candidate);
        }
    }
    std::sort(believed.begin(), believed.end(), [](const Candidate& a, const Candidate& b) {
        if (a.assemblies != b.assemblies) {
            return a.assemblies > b.assemblies;
        }
        return a.offset < b.offset;
    });
    body.number("believedOffsets", believed.size());
    // An integer, because a byte offset is a byte offset: as a float it reads "64.0", and
    // a caller that writes at it wants a number it can add.
    body.raw("bestOffset", believed.empty() ? "null" : std::to_string(believed[0].offset));

    JsonBody offsets;
    for (size_t index = 0; index < believed.size() && index < kExamples; index++) {
        const Candidate& candidate = believed[index];
        JsonBody one;
        one.number("offset", candidate.offset);
        one.number("assemblies", candidate.assemblies);
        one.number("compared", candidate.compared);
        one.number("moved", candidate.moved);
        one.raw("biggestDelta", number(candidate.biggestDelta));
        one.number("identities", candidate.identities);
        offsets.object(std::to_string(index), one.text());
    }
    body.object("offsets", offsets.text());
    return body.finish();
}

} // namespace wiiuport::title
