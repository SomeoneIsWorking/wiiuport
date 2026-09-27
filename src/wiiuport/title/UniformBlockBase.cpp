#include "wiiuport/title/UniformBlockBase.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <utility>

namespace wiiuport::title {

void UniformBlockBase::publish(uint32_t object, uint32_t offset, uint32_t sizeInBytes) {
    m_bindings.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(m_mutex);
    if (m_pending) {
        // A second binding arrived before its assembly. That is a binding the assembly hook
        // did not follow -- a draw with no uniform upload, say -- and the earlier one is not
        // going to be paired with anything. Counted, because a silent overwrite is a pairing
        // that quietly went wrong and the count is how often.
        m_pendingOverwritten.fetch_add(1, std::memory_order_relaxed);
    }
    m_pending = true;
    m_pendingObject = object;
    m_pendingOffset = offset;
    m_pendingSize = sizeInBytes;
}

void UniformBlockBase::observe(const std::vector<uint32_t>& blockSources) {
    m_assemblies.fetch_add(1, std::memory_order_relaxed);
    uint32_t offset = 0;
    {
        std::scoped_lock lock(m_mutex);
        if (!m_pending) {
            return;
        }
        offset = m_pendingOffset;
        m_pending = false;
    }
    m_assembliesWithABinding.fetch_add(1, std::memory_order_relaxed);
    // Every address contributes a candidate, because nothing says which of a draw's blocks is
    // the one the binder named. That over-generates on purpose: a real base appears once per
    // binding whichever pairing produced it, and a wrong one does not repeat.
    std::vector<uint32_t> candidates;
    candidates.reserve(blockSources.size() / 2);
    for (size_t index = 0; index + 1 < blockSources.size(); index += 2) {
        candidates.push_back(blockSources[index + 1] - offset);
    }
    m_candidates.fetch_add(candidates.size(), std::memory_order_relaxed);
    std::scoped_lock lock(m_mutex);
    for (uint32_t candidate : candidates) {
        auto found = m_bases.find(candidate);
        if (found == m_bases.end()) {
            if (m_bases.size() >= kMaxBases) {
                m_basesRefused++;
                continue;
            }
            m_bases.emplace(candidate, 1);
        } else {
            found->second++;
        }
    }
}

UniformBlockBase::Tally UniformBlockBase::tally() const {
    Tally out;
    out.bindings = m_bindings.load();
    out.assemblies = m_assemblies.load();
    out.assembliesWithABinding = m_assembliesWithABinding.load();
    out.candidates = m_candidates.load();
    out.pendingOverwritten = m_pendingOverwritten.load();
    return out;
}

uint32_t UniformBlockBase::bestBase(uint64_t* count) const {
    std::scoped_lock lock(m_mutex);
    return bestBaseLocked(count);
}

uint32_t UniformBlockBase::bestBaseLocked(uint64_t* count) const {
    *count = 0;
    if (m_bases.empty()) {
        return 0;
    }
    uint64_t total = 0;
    uint32_t best = 0;
    uint64_t bestCount = 0;
    for (const auto& [base, seen] : m_bases) {
        total += seen;
        if (seen > bestCount) {
            bestCount = seen;
            best = base;
        }
    }
    // A share of a corpus that is mostly wrong pairings, not simply the largest count. The
    // largest count alone is a guess with a number on it; a large *share* is a property only a
    // base the title really uses has.
    if (total == 0 || static_cast<double>(bestCount) / static_cast<double>(total) < kBeliefShare) {
        return 0;
    }
    *count = bestCount;
    return best;
}

std::string UniformBlockBase::json() const {
    const Tally t = tally();
    std::scoped_lock lock(m_mutex);
    uint64_t total = 0;
    for (const auto& [base, seen] : m_bases) {
        total += seen;
    }
    std::vector<std::pair<uint32_t, uint64_t>> ordered(m_bases.begin(), m_bases.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) {
            return left.second > right.second;
        }
        return left.first < right.first;
    });

    uint64_t bestCount = 0;
    const uint32_t best = bestBaseLocked(&bestCount);

    JsonBody body;
    body.number("bindings", t.bindings);
    body.number("assemblies", t.assemblies);
    body.number("assembliesWithABinding", t.assembliesWithABinding);
    body.number("candidates", t.candidates);
    body.number("bindingsOverwrittenBeforeAssembly", t.pendingOverwritten);
    body.number("distinctBases", m_bases.size());
    body.number("basesRefused", m_basesRefused);
    body.raw("beliefShare", JsonBody::real(kBeliefShare));
    // The answer, with its share, and a null rather than the best candidate when the share did
    // not clear the bar -- a base named from four sightings out of 400,000 is a guess.
    if (best == 0) {
        body.raw("base", "null");
        body.raw("baseShare",
                 total == 0 ? "null"
                            : JsonBody::real(static_cast<double>(bestCount == 0 ? 0 : bestCount) /
                                             static_cast<double>(total)));
    } else {
        body.number("base", best);
        body.number("baseSeen", bestCount);
        body.raw("baseShare",
                 JsonBody::real(static_cast<double>(bestCount) / static_cast<double>(total)));
    }

    // The histogram, because a base that recurs is a fact about a distribution and not about a
    // maximum: a reader must be able to see whether the leader is far ahead or barely ahead.
    JsonBody entries;
    size_t shown = 0;
    for (const auto& [base, seen] : ordered) {
        if (shown >= kExamples) {
            break;
        }
        JsonBody one;
        one.number("base", base);
        one.number("seen", seen);
        one.raw("share", JsonBody::real(static_cast<double>(seen) / static_cast<double>(total)));
        entries.object(std::to_string(shown), one.text());
        shown++;
    }
    body.object("candidatesByBase", entries.text());
    return body.finish();
}

} // namespace wiiuport::title
