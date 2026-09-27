#include "wiiuport/title/UniformBlockRing.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <optional>

namespace wiiuport::title {

UniformBlockRing::UniformBlockRing(ReadWords readWords, Frame frame)
    : m_readWords(readWords), m_frame(frame) {
}

uint64_t UniformBlockRing::hashOf(const std::vector<uint32_t>& words) {
    uint64_t hash = kFnvOffset;
    for (uint32_t word : words) {
        hash ^= word;
        hash *= kFnvPrime;
    }
    return hash;
}

void UniformBlockRing::bind(uint32_t object, uint32_t address, uint32_t sizeInBytes) {
    m_bindings.fetch_add(1, std::memory_order_relaxed);
    if (object == 0 || address == 0) {
        return;
    }
    if (sizeInBytes == 0 || sizeInBytes > kMaxBlockBytes) {
        m_oversize.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t words = (sizeInBytes + 3) / 4;
    std::vector<uint32_t> buffer(words, 0);
    if (!m_readWords(address, buffer.data(), words)) {
        m_unreadable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t hash = hashOf(buffer);
    const uint64_t frame =
        m_frame != nullptr
            ? m_frame()
            : (m_frameCounter == nullptr ? 0 : m_frameCounter->load(std::memory_order_relaxed));

    // The re-read, and it is the whole of the measurement: the PREVIOUS sample's address, read
    // now, compared with the hash taken then. A title that overwrites its uniform block in
    // place gives a different hash here, and that is the answer.
    std::optional<Sample> previous;
    {
        std::scoped_lock lock(m_mutex);
        auto known = std::find_if(m_tracked.begin(), m_tracked.end(), [object](const Tracked& one) {
            return one.object == object;
        });
        if (known == m_tracked.end()) {
            if (m_tracked.size() >= kObjects) {
                m_refused.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            m_tracked.push_back(Tracked{object, {}});
            known = m_tracked.end() - 1;
        }
        if (!known->samples.empty() && known->samples.back().frame == frame) {
            // Two binds of one object inside one tick. That is one tick's contents seen twice,
            // and comparing them would answer "does a block change within a tick", which is not
            // the question.
            return;
        }
        if (known->samples.size() >= kSamplesPerObject) {
            known->samples.erase(known->samples.begin());
        }
        if (known->samples.size() == 1) {
            previous = known->samples.front();
        }
    }
    if (previous.has_value()) {
        const uint32_t previousWords = (previous->sizeInBytes + 3) / 4;
        std::vector<uint32_t> before(previousWords, 0);
        if (m_readWords(previous->address, before.data(), previousWords)) {
            const uint64_t stillHash = hashOf(before);
            previous->read = true;
            previous->stillPresent = stillHash == previous->hash;
            previous->previous = stillHash;
        }
    }

    std::scoped_lock lock(m_mutex);
    auto known = std::find_if(m_tracked.begin(), m_tracked.end(), [object](const Tracked& one) {
        return one.object == object;
    });
    if (known == m_tracked.end()) {
        return;
    }
    // Each sample carries its OWN comparison with its predecessor, rather than the tally
    // walking pairs. The first version pushed the predecessor as well and then walked, so one
    // comparison was counted twice -- and the tally's job is to count comparisons, which is a
    // per-sample fact.
    Sample sample;
    sample.frame = frame;
    sample.address = address;
    sample.sizeInBytes = sizeInBytes;
    sample.hash = hash;
    if (previous.has_value()) {
        sample.compared = true;
        sample.read = previous->read;
        sample.stillPresent = previous->stillPresent;
        sample.previous = previous->previous;
    }
    known->samples.push_back(sample);
}

UniformBlockRing::Tally UniformBlockRing::tally() const {
    std::scoped_lock lock(m_mutex);
    Tally out;
    out.objects = m_tracked.size();
    out.refused = m_refused.load();
    for (const Tracked& one : m_tracked) {
        for (const Sample& sample : one.samples) {
            // A first sample carries no comparison, and a comparison that did not happen is not
            // a negative answer -- so it is counted as neither.
            if (!sample.compared) {
                // A first sample. Counted as nothing at all, not even as a failed comparison.
                continue;
            }
            if (!sample.read) {
                out.unreadable++;
                continue;
            }
            out.pairsCompared++;
            if (sample.stillPresent) {
                out.stillPresent++;
            } else {
                out.overwritten++;
            }
        }
    }
    return out;
}

std::string UniformBlockRing::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.number("bindings", m_bindings.load());
    body.number("blocksOversize", m_oversize.load());
    body.number("blocksUnreadable", m_unreadable.load());
    body.number("objectsRefused", m_refused.load());
    body.number("objectsTracked", m_tracked.size());
    body.number("maxBlockBytes", kMaxBlockBytes);
    body.string("schedule", m_frame == nullptr && m_frameCounter == nullptr ? "none" : "perFrame");

    uint64_t compared = 0;
    uint64_t stillPresent = 0;
    uint64_t overwritten = 0;
    uint64_t unreadable = 0;
    uint64_t alternating = 0;
    for (const Tracked& one : m_tracked) {
        for (const Sample& sample : one.samples) {
            if (!sample.compared || !sample.read) {
                if (sample.compared) {
                    unreadable++;
                }
                continue;
            }
            compared++;
            if (sample.stillPresent) {
                stillPresent++;
            } else {
                overwritten++;
            }
        }
        // Double buffering, measured: consecutive samples of one object at different
        // addresses. Walked over the samples rather than the comparisons, because the addresses
        // are a property of the samples.
        for (size_t index = 1; index < one.samples.size(); index++) {
            if (one.samples[index - 1].address != one.samples[index].address) {
                alternating++;
            }
        }
    }
    // The answer, with its denominator, at the top: this is the objective's second question and
    // it should not have to be assembled from the table below.
    body.number("pairsCompared", compared);
    body.number("previousTickStillPresent", stillPresent);
    body.number("previousTickOverwritten", overwritten);
    body.number("comparisonsUnreadable", unreadable);
    // Double buffering, measured: consecutive samples of one object that differ in address.
    body.number("consecutivePairsWithDifferentAddress", alternating);

    JsonBody objects;
    size_t shown = 0;
    for (const Tracked& one : m_tracked) {
        if (shown >= kObjects) {
            break;
        }
        JsonBody entry;
        entry.number("object", one.object);
        entry.number("samples", one.samples.size());
        // The addresses, because a title that alternates between two for one object is
        // double-buffering and that is the structure that does preserve the previous tick.
        JsonBody addresses;
        for (size_t index = 0; index < one.samples.size(); index++) {
            JsonBody sample;
            sample.number("frame", one.samples[index].frame);
            sample.number("address", one.samples[index].address);
            sample.number("sizeInBytes", one.samples[index].sizeInBytes);
            // Three states, named as three: nothing to compare, a comparison that failed, and
            // a comparison that ran.
            sample.string("previousStillPresent",
                          !one.samples[index].compared
                              ? "noPredecessor"
                              : (!one.samples[index].read
                                     ? "unreadable"
                                     : (one.samples[index].stillPresent ? "true" : "false")));
            addresses.object(std::to_string(index), sample.text());
        }
        entry.object("samplesDetail", addresses.text());
        objects.object(std::to_string(shown), entry.text());
        shown++;
    }
    body.object("objects", objects.text());
    return body.finish();
}

} // namespace wiiuport::title
