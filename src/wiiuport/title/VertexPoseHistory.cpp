#include "wiiuport/title/VertexPoseHistory.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wiiuport::title {

namespace {

// One component of a position, as a difference in the attribute's own units.
//
// The bytes are read in the byte order the draw declared. `endianSwap` is carried into the
// report rather than guessed at here: a byte-wise count of differing bytes is
// order-independent, but a magnitude read as a float is not, and a float read with the wrong
// order is a plausible number describing nothing. So the magnitude is only reported when the
// attribute is four bytes of little-endian float, and the report says which of the two it gave.
bool readFloat(const uint8_t* bytes, float* out) {
    uint32_t word = 0;
    std::memcpy(&word, bytes, sizeof(word));
    std::memcpy(out, &word, sizeof(*out));
    return std::isfinite(*out);
}

} // namespace

VertexPoseHistory::VertexPoseHistory(const ObjectIdentityScope* scope,
                                     const DrawAttributeCensus* attributes,
                                     const std::atomic<uint64_t>* frames)
    : m_scope(scope), m_attributes(attributes), m_frames(frames) {
}

void VertexPoseHistory::onDrawRecorded(const LatteFrameHooks::DrawPrepared& draw) {
    m_drawsSeen.fetch_add(1, std::memory_order_relaxed);
    // The position attribute, named by the census from the title's own tables. Null when the
    // census has not settled one, and then nothing is recorded rather than a guess being
    // sampled.
    if (m_attributes == nullptr) {
        m_drawsWithoutPosition.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const DrawAttributeCensus::Position at = m_attributes->position();
    if (!at.known || at.buffer >= draw.vertexBufferCount) {
        m_drawsWithoutPosition.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const LatteFrameHooks::DrawPrepared::VertexBuffer& buffer = draw.vertexBuffers[at.buffer];
    if (buffer.stride == 0 || buffer.stride < at.offset + at.sizeInBytes) {
        // A stride that cannot hold the attribute. Counted, and not sampled: a copy that walks
        // a stride shorter than the attribute it is reading overlaps itself and produces
        // numbers that look like a mesh and are not one.
        m_drawsWithoutPosition.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t vertices = buffer.sizeInBytes / buffer.stride;
    const uint64_t total = static_cast<uint64_t>(vertices) * at.sizeInBytes;
    if (total == 0 || total > kMaxPositionBytes) {
        m_drawsOversize.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t node = m_scope == nullptr ? 0u : m_scope->current();
    if (node == 0) {
        m_drawsWithoutNode.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t frame = m_frames == nullptr ? 0 : m_frames->load(std::memory_order_relaxed);

    // Copy while the draw's buffer is valid: the pointer is good only for this callback, and a
    // history that stored the address would be a history of addresses.
    std::vector<uint8_t> bytes(static_cast<size_t>(total), 0);
    const auto* base = static_cast<const uint8_t*>(buffer.data) + at.offset;
    for (uint32_t vertex = 0; vertex < vertices; vertex++) {
        std::memcpy(bytes.data() + static_cast<size_t>(vertex) * at.sizeInBytes,
                    base + static_cast<size_t>(vertex) * buffer.stride, at.sizeInBytes);
    }

    std::scoped_lock lock(m_mutex);
    auto known = std::find_if(m_nodes.begin(), m_nodes.end(), [node](const Node& one) {
        return one.address == node;
    });
    if (known == m_nodes.end()) {
        if (m_nodes.size() >= kNodes) {
            m_refused++;
            return;
        }
        m_nodes.push_back(Node{});
        known = m_nodes.end() - 1;
        known->address = node;
    }
    // One sample per frame. A node drawn forty times in one frame is forty views of one
    // instant, and comparing them would report a static field as a static field for the wrong
    // reason.
    if (!known->samples.empty() && known->samples.back().frame == frame) {
        return;
    }
    if (known->samples.size() >= kSamplesPerNode) {
        known->samples.erase(known->samples.begin());
    }
    known->samples.push_back(Node::Sample{frame, buffer.stride, std::move(bytes)});
    known->stride = buffer.stride;
    known->componentBytes = at.sizeInBytes;
    known->offset = at.offset;
}

VertexPoseHistory::Verdict VertexPoseHistory::verdictOf(const Node& node, uint64_t* differingBytes,
                                                        float* biggestDelta) {
    *differingBytes = 0;
    *biggestDelta = 0.0f;
    if (node.samples.size() < kSamplesPerNode) {
        return Verdict::OneSample;
    }
    const Node::Sample& before = node.samples[0];
    const Node::Sample& after = node.samples[1];
    if (before.stride != after.stride || before.bytes.size() != after.bytes.size()) {
        // The draw's shape changed between the two ticks. Not "identical" and not "blendable":
        // there is no correspondence between the two samples' components to interpolate, and
        // a lerp across it would be arithmetic on unrelated numbers.
        return Verdict::Uncomparable;
    }
    const size_t components =
        after.bytes.size() / (node.componentBytes / 4 == 0 ? 1 : node.componentBytes / 4);
    for (size_t byte = 0; byte < after.bytes.size(); byte++) {
        if (before.bytes[byte] != after.bytes[byte]) {
            (*differingBytes)++;
        }
    }
    // The magnitude, component by component, where the component is a float. A twelve-byte
    // attribute is three of them.
    if (node.componentBytes % 4 == 0) {
        const size_t perVertex = node.componentBytes;
        const size_t vertices = after.bytes.size() / (perVertex == 0 ? 1 : perVertex);
        for (size_t vertex = 0; vertex < vertices; vertex++) {
            for (size_t offset = 0; offset + 4 <= perVertex; offset += 4) {
                float a = 0.0f;
                float b = 0.0f;
                const size_t at = vertex * perVertex + offset;
                if (!readFloat(before.bytes.data() + at, &a) ||
                    !readFloat(after.bytes.data() + at, &b)) {
                    continue;
                }
                *biggestDelta = std::max(*biggestDelta, std::fabs(b - a));
            }
        }
    }
    if (*differingBytes == 0) {
        return Verdict::Identical;
    }
    return Verdict::Blendable;
}

const char* VertexPoseHistory::nameOf(Verdict verdict) {
    switch (verdict) {
    case Verdict::OneSample:
        return "oneSample";
    case Verdict::Identical:
        return "identical";
    case Verdict::Blendable:
        return "blendable";
    case Verdict::Uncomparable:
        return "uncomparable";
    }
    return "unknown";
}

VertexPoseHistory::Tally VertexPoseHistory::tally() const {
    std::scoped_lock lock(m_mutex);
    Tally out;
    out.tracked = m_nodes.size();
    out.refused = m_refused;
    for (const Node& node : m_nodes) {
        uint64_t differing = 0;
        float biggest = 0.0f;
        switch (verdictOf(node, &differing, &biggest)) {
        case Verdict::Blendable:
            out.blendable++;
            break;
        case Verdict::Identical:
            out.identical++;
            break;
        case Verdict::Uncomparable:
            out.uncomparable++;
            break;
        case Verdict::OneSample:
            break;
        }
    }
    return out;
}

std::string VertexPoseHistory::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.number("drawsSeen", m_drawsSeen.load());
    body.number("drawsWithoutPosition", m_drawsWithoutPosition.load());
    body.number("drawsOversize", m_drawsOversize.load());
    body.number("drawsWithoutNode", m_drawsWithoutNode.load());
    body.string("schedule", m_frames == nullptr ? "perBind" : "perFrame");
    body.number("nodesTracked", m_nodes.size());
    body.number("nodesRefused", m_refused);
    body.number("maxPositionBytes", kMaxPositionBytes);

    // The headline, with its denominator, because "can this work at all" is one number and it
    // should not have to be assembled from the table below.
    uint64_t blendable = 0;
    uint64_t identical = 0;
    uint64_t uncomparable = 0;
    uint64_t oneSample = 0;
    for (const Node& node : m_nodes) {
        uint64_t differing = 0;
        float biggest = 0.0f;
        switch (verdictOf(node, &differing, &biggest)) {
        case Verdict::Blendable:
            blendable++;
            break;
        case Verdict::Identical:
            identical++;
            break;
        case Verdict::Uncomparable:
            uncomparable++;
            break;
        case Verdict::OneSample:
            oneSample++;
            break;
        }
    }
    body.number("blendable", blendable);
    body.number("identical", identical);
    body.number("uncomparable", uncomparable);
    body.number("oneSample", oneSample);

    JsonBody nodes;
    size_t shown = 0;
    for (const Node& node : m_nodes) {
        if (shown >= kNodes) {
            break;
        }
        uint64_t differing = 0;
        float biggest = 0.0f;
        const Verdict verdict = verdictOf(node, &differing, &biggest);
        const size_t bytes = node.samples.empty() ? 0 : node.samples.back().bytes.size();
        JsonBody one;
        one.number("node", node.address);
        one.string("verdict", nameOf(verdict));
        one.number("samples", node.samples.size());
        one.number("fromFrame", node.samples.empty() ? 0 : node.samples.front().frame);
        one.number("toFrame", node.samples.size() < 2 ? 0 : node.samples.back().frame);
        one.number("framesApart", node.samples.size() < 2
                                      ? 0
                                      : node.samples.back().frame - node.samples.front().frame);
        one.number("stride", node.stride);
        one.number("componentBytes", node.componentBytes);
        one.number("offsetInStride", node.offset);
        one.number("vertices", node.stride == 0 ? 0 : bytes / node.componentBytes);
        one.number("positionBytes", bytes);
        one.number("differingBytes", differing);
        one.raw("biggestComponentDelta", JsonBody::real(static_cast<double>(biggest)));
        nodes.object(std::to_string(shown), one.text());
        shown++;
    }
    body.object("nodes", nodes.text());
    return body.finish();
}

} // namespace wiiuport::title
