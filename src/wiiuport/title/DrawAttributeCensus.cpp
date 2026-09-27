#include "wiiuport/title/DrawAttributeCensus.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <tuple>

namespace wiiuport::title {

namespace {

// A signature, as a comparable value. One comparison rule for the whole census: the ordering
// used to sort the report and the one used to fold a draw into a node are the same, so a
// signature cannot be counted as two because it was reached two ways.
// **The stride is part of the identity, and leaving it out is what produced one global
// position.** The signature recorded a stride and the key ignored it, so a stride-32 draw and a
// stride-20 draw whose attribute fields agreed folded into one signature and the majority
// counted both. The census named a single position at a single offset, and the real run showed
// the cost: five objects compared cleanly at stride 32 while two at strides of 20 and 64
// reported 18 and 60 components out of range, because offset 0 of a 20-byte stride is not a
// position. A title has several vertex layouts and each packs its position differently, so the
// stride belongs in the identity of the thing being counted.
using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;

// The share of components read that were implausible as positions. A layout nobody has read
// anything at has read nothing implausible, which is not the same as having been checked -- so
// this is only ever consulted where componentsRead is non-zero.
double shareOf(uint64_t read, uint64_t implausible) {
    if (read == 0) {
        return 0.0;
    }
    return static_cast<double>(implausible) / static_cast<double>(read);
}

Key keyOf(const DrawAttributeCensus::Signature& signature) {
    return {signature.semanticId, signature.format, signature.sizeInBytes, signature.perInstance,
            signature.buffer,     signature.offset, signature.stride};
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
        // The magnitudes at this offset, read now while the buffer is valid. A count of how
        // many objects agree on a signature cannot tell a position from bytes that share its
        // stride: every one of seven stride-20 objects agreed, and reading them gave 1e+38 for
        // some and a believable 0.107 for others. Only the values can.
        uint64_t read = 0;
        uint64_t implausible = 0;
        const LatteFrameHooks::DrawPrepared::VertexBuffer& source =
            draw.vertexBuffers[attribute.buffer];
        if (source.data != nullptr) {
            const uint32_t perVertex = attribute.sizeInBytes / 4;
            const uint32_t vertices = source.stride == 0 ? 0 : source.sizeInBytes / source.stride;
            const auto* base = static_cast<const uint8_t*>(source.data) + attribute.offset;
            for (uint32_t vertex = 0; vertex < vertices && vertex < kMagnitudeSamples; vertex++) {
                for (uint32_t component = 0; component < perVertex; component++) {
                    uint32_t word = 0;
                    std::memcpy(&word, base + (vertex * source.stride) + component * 4,
                                sizeof(word));
                    float value = 0.0f;
                    std::memcpy(&value, &word, sizeof(value));
                    read++;
                    // Three ways a value is not a position. Above the ceiling: a projection
                    // constant. Non-finite: a byte pattern. Below the floor **and non-zero**: a
                    // denormal, which is 1.7e-38 and is not a position in any world -- and the
                    // non-zero matters, because a vertex AT THE ORIGIN is exactly zero and is a
                    // perfectly good position. The first version of this floor had no non-zero
                    // clause and refused every layout whose fixtures put their vertices at the
                    // origin, which is most of them.
                    const float magnitude = std::fabs(value);
                    if (!std::isfinite(value) || magnitude > kComponentCeiling ||
                        (magnitude != 0.0f && magnitude < kComponentFloor)) {
                        implausible++;
                    }
                }
            }
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
        seen.componentsRead = read;
        seen.componentsImplausible = implausible;

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
        known->componentsRead += read;
        known->componentsImplausible += implausible;
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
    return positionForLocked(0);
}

uint64_t DrawAttributeCensus::nodesAtStrideLocked(uint32_t stride) const {
    // Distinct objects holding a position-sized attribute at this stride. A stride of zero asks
    // over every stride, which is the question `position()` asks -- and the answer to it is a
    // mixture of layouts, which is why a draw should ask about its own.
    std::map<uint32_t, std::set<uint32_t>> byStride;
    for (const Node& node : m_nodes) {
        if (node.address == 0) {
            continue;
        }
        for (const Signature& signature : node.signatures) {
            if (signature.sizeInBytes == kPositionBytes ||
                signature.sizeInBytes == kPositionBytesPadded) {
                byStride[signature.stride].insert(node.address);
            }
        }
    }
    if (stride != 0) {
        const auto found = byStride.find(stride);
        return found == byStride.end() ? 0 : static_cast<uint64_t>(found->second.size());
    }
    uint64_t total = 0;
    for (const auto& [one, objects] : byStride) {
        total += objects.size();
    }
    return total;
}

DrawAttributeCensus::Position DrawAttributeCensus::positionForLocked(uint32_t stride) const {
    // **The objects whose draws had this stride, and only those.** A position named across
    // layouts is one layout's answer presented as the title's.
    const uint64_t nodes = nodesAtStrideLocked(stride);
    if (nodes < 2) {
        // One object cannot agree with another, and the cross-object bar is the whole of the
        // census. Its signatures are reported; none is named.
        return {};
    }
    const uint64_t needed = nodes / 2 + 1;
    std::map<Key, std::set<uint32_t>> across;
    std::map<Key, uint64_t> implausible;
    std::map<Key, uint64_t> read;
    for (const Node& node : m_nodes) {
        if (node.address == 0) {
            continue;
        }
        for (const Signature& signature : node.signatures) {
            if (stride != 0 && signature.stride != stride) {
                continue;
            }
            if (signature.sizeInBytes != kPositionBytes &&
                signature.sizeInBytes != kPositionBytesPadded) {
                continue;
            }
            across[keyOf(signature)].insert(node.address);
            implausible[keyOf(signature)] += signature.componentsImplausible;
            read[keyOf(signature)] += signature.componentsRead;
        }
    }
    const std::vector<Position> clearing = candidatesClearingLocked(stride);
    if (clearing.empty()) {
        return {};
    }
    return clearing.front();
}

std::vector<DrawAttributeCensus::Position>
DrawAttributeCensus::candidatesClearingLocked(uint32_t stride) const {
    // The objects whose draws had this stride, and only those: a position named across layouts
    // is one layout's answer presented as the title's.
    const uint64_t nodes = nodesAtStrideLocked(stride);
    if (nodes < 2) {
        // One object cannot agree with another, and the cross-object bar is the whole of the
        // census. Its signatures are reported; none is named.
        return {};
    }
    const uint64_t needed = nodes / 2 + 1;
    std::map<Key, std::set<uint32_t>> across;
    std::map<Key, uint64_t> implausible;
    std::map<Key, uint64_t> read;
    for (const Node& node : m_nodes) {
        if (node.address == 0) {
            continue;
        }
        for (const Signature& signature : node.signatures) {
            if (stride != 0 && signature.stride != stride) {
                continue;
            }
            if (signature.sizeInBytes != kPositionBytes &&
                signature.sizeInBytes != kPositionBytesPadded) {
                continue;
            }
            across[keyOf(signature)].insert(node.address);
            implausible[keyOf(signature)] += signature.componentsImplausible;
            read[keyOf(signature)] += signature.componentsRead;
        }
    }
    // The count bar AND the magnitude bar, and the magnitude bar is a share. Seven objects
    // agreeing that the stride-20 position is at offset 0 is not enough, because those twelve
    // bytes are partly position and partly the eight other bytes in the stride. But refusing a
    // layout because ONE component in ONE draw was implausible refuses four of the title's seven
    // layouts, which is a threshold in the wrong place rather than a bar.
    std::vector<std::pair<Key, uint64_t>> kept;
    for (const auto& [key, objects] : across) {
        if (objects.size() < needed) {
            continue;
        }
        if (shareOf(read.at(key), implausible.at(key)) > kImplausibleShareCeiling) {
            continue;
        }
        kept.emplace_back(key, objects.size());
    }
    // The stated tie-break: lowest semantic index, then lowest offset. It is a rule and not a
    // measurement, and the report carries every candidate beside the one it chose so a reader can
    // see that a choice was made.
    std::sort(kept.begin(), kept.end(), [](const auto& left, const auto& right) {
        if (std::get<0>(left.first) != std::get<0>(right.first)) {
            return std::get<0>(left.first) < std::get<0>(right.first);
        }
        if (std::get<5>(left.first) != std::get<5>(right.first)) {
            return std::get<5>(left.first) < std::get<5>(right.first);
        }
        return left.second > right.second;
    });
    std::vector<Position> out;
    for (const auto& [key, objects] : kept) {
        Position one;
        one.semanticId = std::get<0>(key);
        one.format = std::get<1>(key);
        one.sizeInBytes = std::get<2>(key);
        one.perInstance = std::get<3>(key);
        one.buffer = std::get<4>(key);
        one.offset = std::get<5>(key);
        one.stride = std::get<6>(key);
        one.objectsAgreeing = objects;
        one.known = true;
        out.push_back(one);
    }
    return out;
}

DrawAttributeCensus::Position DrawAttributeCensus::positionFor(uint32_t stride) const {
    std::scoped_lock lock(m_mutex);
    return positionForLocked(stride);
}

std::vector<uint32_t> DrawAttributeCensus::stridesWithPosition() const {
    std::scoped_lock lock(m_mutex);
    return stridesWithPositionLocked();
}

std::vector<uint32_t> DrawAttributeCensus::stridesWithPositionLocked() const {
    std::set<uint32_t> strides;
    for (const Node& node : m_nodes) {
        for (const Signature& signature : node.signatures) {
            if (signature.sizeInBytes == kPositionBytes ||
                signature.sizeInBytes == kPositionBytesPadded) {
                strides.insert(signature.stride);
            }
        }
    }
    return {strides.begin(), strides.end()};
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
    // How many components read as something a position is not, per signature. Collected over
    // every object, because the magnitude bar is a property of the layout rather than of one
    // draw.
    std::map<Key, uint64_t> implausibleCounts;
    std::map<Key, uint64_t> readCounts;
    for (const Node& node : m_nodes) {
        for (const Signature& signature : node.signatures) {
            implausibleCounts[keyOf(signature)] += signature.componentsImplausible;
            readCounts[keyOf(signature)] += signature.componentsRead;
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
        one.number("stride", std::get<6>(key));
        one.number("nodes", counts.first);
        one.number("draws", counts.second);
        one.number("componentsRead", readCounts.at(key));
        one.number("componentsImplausible", implausibleCounts.at(key));
        // The share and the bar's own value, so a reader can see how close a layout is to being
        // refused rather than only whether it was.
        one.raw("implausibleShare",
                JsonBody::real(shareOf(readCounts.at(key), implausibleCounts.at(key))));
        one.raw("implausibleShareCeiling", JsonBody::real(kImplausibleShareCeiling));
        one.string("magnitudeBar", shareOf(readCounts.at(key), implausibleCounts.at(key)) <=
                                           kImplausibleShareCeiling
                                       ? "pass"
                                       : "fail");
        one.raw("positionSized",
                (std::get<2>(key) == kPositionBytes || std::get<2>(key) == kPositionBytesPadded)
                    ? "true"
                    : "false");
        one.raw("named", counts.first >= needed && needed > 0 ? "true" : "false");
        entries.object(std::to_string(shown), one.text());
        shown++;
    }
    body.object("signatures", entries.text());
    // The layouts, because "one position for the title" was the thing that hid them. A reader
    // who sees a single named position and a list of four strides knows immediately that the
    // one answer is one layout's.
    JsonBody layouts;
    const std::vector<uint32_t> strides = stridesWithPositionLocked();
    for (size_t index = 0; index < strides.size(); index++) {
        const Position one = positionForLocked(strides[index]);
        JsonBody layout;
        layout.number("stride", strides[index]);
        layout.number("objects", nodesAtStrideLocked(strides[index]));
        // How many candidates cleared, the rule that chose between them, and all of them. Two
        // runs of the same title named this layout's position differently because more than one
        // position-sized attribute has plausible values, so the reader is entitled to see that a
        // choice was made and to see the alternatives.
        const std::vector<Position> clearing = candidatesClearingLocked(strides[index]);
        layout.number("candidatesClearing", clearing.size());
        layout.string("tieBreak", "lowestSemanticThenOffset");
        JsonBody also;
        for (size_t rank = 0; rank < clearing.size() && rank < kExamples; rank++) {
            JsonBody candidate;
            candidate.number("semanticId", clearing[rank].semanticId);
            candidate.number("sizeInBytes", clearing[rank].sizeInBytes);
            candidate.number("offsetInStride", clearing[rank].offset);
            candidate.number("objectsAgreeing", clearing[rank].objectsAgreeing);
            also.object(std::to_string(rank), candidate.text());
        }
        layout.object("candidates", also.text());
        layout.raw("positionKnown", one.known ? "true" : "false");
        if (one.known) {
            layout.number("semanticId", one.semanticId);
            layout.number("sizeInBytes", one.sizeInBytes);
            layout.number("buffer", one.buffer);
            layout.number("offsetInStride", one.offset);
        }
        layouts.object(std::to_string(index), layout.text());
    }
    body.object("layouts", layouts.text());
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
