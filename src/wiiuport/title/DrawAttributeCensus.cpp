#include "wiiuport/title/DrawAttributeCensus.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace wiiuport::title {

namespace {

// A signature, as a comparable value. One comparison rule for the whole census: the ordering
// used to sort the report and the one used to fold a draw into a node are the same, so a
// signature cannot be counted as two because it was reached two ways.
using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;

Key keyOf(const DrawAttributeCensus::Signature& signature) {
    return {signature.semanticId,  signature.format, signature.sizeInBytes,
            signature.perInstance, signature.buffer, signature.offset};
}

const char* hexValue(uint32_t value) {
    static thread_local std::array<char, 11> text{};
    std::snprintf(text.data(), text.size(), "0x%08x", value);
    return text.data();
}

} // namespace

bool DrawAttributeCensus::Signature::operator<(const Signature& other) const {
    return keyOf(*this) < keyOf(other);
}

void DrawAttributeCensus::onDrawRecorded(const LatteFrameHooks::DrawPrepared& draw) {
    m_draws.fetch_add(1, std::memory_order_relaxed);
    if (draw.vertexAttributeCount == 0) {
        m_drawsWithoutAttributes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The node this draw belongs to, from the slot the binder published. Read here as well as
    // at the uniform assembly: both are points where the title's own code knows which object it
    // is inside, and the report counts every read so its coverage is the coverage of both.
    const uint32_t object = m_scope == nullptr ? 0u : m_scope->current();
    if (object == 0) {
        m_drawsWithoutIdentity.fetch_add(1, std::memory_order_relaxed);
    }

    // Whether this node is tracked and has draws left is a shared decision, made under the
    // lock. The walk of the draw's own attribute table is not: the table is the title's, valid
    // only during the callback, and it is read into the census before the lock is taken again.
    {
        std::scoped_lock lock(m_mutex);
        auto known = std::find_if(m_nodes.begin(), m_nodes.end(), [object](const Node& one) {
            return one.address == object;
        });
        if (known == m_nodes.end()) {
            if (m_nodes.size() >= kNodes) {
                m_nodesRefused.fetch_add(1, std::memory_order_relaxed);
                known = m_nodes.end();
            } else {
                m_nodes.push_back(Node{object, 0, {}});
                known = m_nodes.end() - 1;
            }
        }
        if (known != m_nodes.end()) {
            known->draws++;
        }
    }

    for (uint32_t index = 0; index < draw.vertexAttributeCount; index++) {
        const LatteFrameHooks::DrawPrepared::VertexAttribute& attribute =
            draw.vertexAttributes[index];
        m_attributesRead.fetch_add(1, std::memory_order_relaxed);
        if (attribute.buffer >= draw.vertexBufferCount) {
            // An attribute naming a buffer the draw does not have. Counted, and not read
            // further: the geometry needed to size a substitution is not there, and a
            // signature with a stride of zero would be a signature nobody could act on.
            m_attributesOutOfRange.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        Signature seen;
        seen.semanticId = attribute.semanticId;
        seen.format = attribute.format;
        seen.sizeInBytes = attribute.sizeInBytes;
        seen.perInstance = attribute.perInstance ? 1u : 0u;
        seen.buffer = attribute.buffer;
        seen.offset = attribute.offset;
        seen.stride = draw.vertexBuffers[attribute.buffer].stride;
        seen.bufferBytes = draw.vertexBuffers[attribute.buffer].sizeInBytes;
        seen.draws = 1;
        seen.nodes = object == 0 ? 0u : 1u;

        std::scoped_lock lock(m_mutex);
        auto node = std::find_if(m_nodes.begin(), m_nodes.end(), [object](const Node& one) {
            return one.address == object;
        });
        if (node == m_nodes.end()) {
            continue;
        }
        // A lower_bound, not a find_if. `find_if` with `seen < one` finds the first element
        // *greater* than the new signature and treats it as the match, so two attributes of one
        // draw were folded into whichever came first in the vector -- and the suite caught a
        // draw whose sixteen-byte position was reported as its twelve-byte normal. The vector
        // is kept sorted, so the search is the one that means what it says.
        auto known = std::lower_bound(node->signatures.begin(), node->signatures.end(), seen);
        if (known == node->signatures.end() || seen < *known) {
            known = node->signatures.insert(known, seen);
        }
        known->draws++;
    }
}

uint64_t DrawAttributeCensus::nodesTrackedLocked() const {
    // Distinct non-zero objects. A draw with no identity published is not an object, and
    // counting the zero address as one would put a phantom in the denominator that every
    // signature divides by.
    uint64_t count = 0;
    for (const Node& node : m_nodes) {
        if (node.address != 0) {
            count++;
        }
    }
    return count;
}

uint64_t DrawAttributeCensus::nodesTracked() const {
    std::scoped_lock lock(m_mutex);
    return nodesTrackedLocked();
}

DrawAttributeCensus::Position DrawAttributeCensus::positionLocked() const {
    const uint64_t nodes = nodesTrackedLocked();
    if (nodes < 2) {
        // One object cannot agree with another, and the cross-object bar is the whole of the
        // census. Its signatures are reported; none is named.
        return {};
    }
    const uint64_t needed = nodes / 2 + 1;
    // Key -> how many distinct objects held it.
    std::map<Key, uint64_t> across;
    for (const Node& node : m_nodes) {
        if (node.address == 0) {
            continue;
        }
        for (const Signature& signature : node.signatures) {
            if (signature.sizeInBytes != kPositionBytes &&
                signature.sizeInBytes != kPositionBytesPadded) {
                continue;
            }
            across[keyOf(signature)]++;
        }
    }
    Position out;
    uint64_t bestObjects = 0;
    for (const auto& [key, count] : across) {
        if (count >= needed && count > bestObjects) {
            bestObjects = count;
            out.semanticId = std::get<0>(key);
            out.format = std::get<1>(key);
            out.sizeInBytes = std::get<2>(key);
            out.perInstance = std::get<3>(key);
            out.buffer = std::get<4>(key);
            out.offset = std::get<5>(key);
            out.known = true;
        }
    }
    return out;
}

DrawAttributeCensus::Position DrawAttributeCensus::position() const {
    std::scoped_lock lock(m_mutex);
    return positionLocked();
}

std::string DrawAttributeCensus::json() const {
    std::scoped_lock lock(m_mutex);
    const uint64_t nodes = nodesTrackedLocked();
    const uint64_t needed = nodes < 2 ? 0 : nodes / 2 + 1;

    JsonBody body;
    body.number("draws", m_draws.load());
    body.number("attributesRead", m_attributesRead.load());
    body.number("drawsWithoutAttributes", m_drawsWithoutAttributes.load());
    body.number("drawsWithoutIdentity", m_drawsWithoutIdentity.load());
    body.number("attributesOutOfRange", m_attributesOutOfRange.load());
    body.number("nodesTracked", nodes);
    body.number("nodesRefused", m_nodesRefused.load());
    body.string("identitySource", m_scope == nullptr ? "none" : "binderObject");
    // The bar, in numbers, so a reader can see what "believed" is worth before believing it.
    body.number("nodesNeeded", needed);
    body.number("positionBytesAccepted", kPositionBytes);
    body.number("positionBytesPaddedAccepted", kPositionBytesPadded);

    // The whole histogram, most-recurring first: a belief without it is a claim.
    std::map<Key, std::pair<uint64_t, uint64_t>> across;
    for (const Node& node : m_nodes) {
        if (node.address == 0) {
            continue;
        }
        for (const Signature& signature : node.signatures) {
            auto& entry = across[keyOf(signature)];
            entry.first++;
            entry.second += signature.draws;
        }
    }
    std::vector<std::pair<Key, std::pair<uint64_t, uint64_t>>> ordered(across.begin(),
                                                                       across.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.second.first != right.second.first) {
            return left.second.first > right.second.first;
        }
        if (left.second.second != right.second.second) {
            return left.second.second > right.second.second;
        }
        return left.first < right.first;
    });
    JsonBody entries;
    size_t shown = 0;
    for (const auto& [key, counts] : ordered) {
        if (shown >= kExamples) {
            break;
        }
        JsonBody one;
        one.number("semanticId", std::get<0>(key));
        one.number("format", std::get<1>(key));
        one.string("formatHex", hexValue(std::get<1>(key)));
        one.number("sizeInBytes", std::get<2>(key));
        one.number("perInstance", std::get<3>(key));
        one.number("buffer", std::get<4>(key));
        one.number("offsetInStride", std::get<5>(key));
        one.number("nodes", counts.first);
        one.number("draws", counts.second);
        one.raw("positionSized",
                (std::get<2>(key) == kPositionBytes || std::get<2>(key) == kPositionBytesPadded)
                    ? "true"
                    : "false");
        one.raw("named", counts.first >= needed && needed > 0 ? "true" : "false");
        entries.object(std::to_string(shown), one.text());
        shown++;
    }
    body.object("signatures", entries.text());
    const Position at = positionLocked();
    body.raw("positionKnown", at.known ? "true" : "false");
    if (at.known) {
        body.number("positionSemanticId", at.semanticId);
        body.number("positionFormat", at.format);
        body.string("positionFormatHex", hexValue(at.format));
        body.number("positionSizeInBytes", at.sizeInBytes);
        body.number("positionBuffer", at.buffer);
        body.number("positionOffsetInStride", at.offset);
        body.number("positionPerInstance", at.perInstance);
    }
    return body.finish();
}

} // namespace wiiuport::title
