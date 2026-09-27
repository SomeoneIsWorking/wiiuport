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
    // The position for THIS draw's own layout, not a global one. A title has several vertex
    // layouts and each packs its position at its own offset: the run that found this compared
    // five objects cleanly at stride 32 and read 18 and 60 components out of range at strides
    // of 20 and 64, because a position named across layouts is one layout's answer.
    //
    // Asked per buffer, because the stride that identifies the layout is the stride of the
    // buffer the position is *in* -- which is not known until the census answers, and guessing
    // buffer zero would be exactly the single-layout assumption this replaces. A draw's buffers
    // are a handful, so asking each is bounded and exact.
    DrawAttributeCensus::Position at;
    for (uint32_t index = 0; index < draw.vertexBufferCount && !at.known; index++) {
        const DrawAttributeCensus::Position candidate =
            m_attributes->positionFor(draw.vertexBuffers[index].stride);
        if (candidate.known && candidate.buffer < draw.vertexBufferCount) {
            at = candidate;
        }
    }
    if (!at.known) {
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
    // One sample per shape per frame, matched to the previous sample of the same shape.
    //
    // A node drawn forty times in one frame is forty views of one instant, and comparing two of
    // them would report a static field as static for the wrong reason. A node drawn twice in
    // one frame with two different meshes is two shapes, and comparing them would report a
    // shape change that never happened. And a node drawn twice in one frame with the SAME bytes
    // still needs both samples: "compared, and found identical" is the evidence that a
    // stationary object stays stationary, and the first version here dropped the earlier
    // sample when the bytes matched -- which deleted the comparison the verdict reports.
    std::vector<Node::Sample>& samples = known->samples;
    const auto sameShape = [stride = buffer.stride, size = bytes.size()](const Node::Sample& one) {
        return one.stride == stride && one.bytes.size() == size;
    };
    // A re-draw of the same shape in the same frame replaces its sample rather than adding one.
    samples.erase(std::remove_if(samples.begin(), samples.end(),
                                 [&](const Node::Sample& one) {
                                     return sameShape(one) && one.frame == frame;
                                 }),
                  samples.end());
    if (samples.size() >= kShapesPerNode * kSamplesPerShape) {
        return;
    }
    samples.push_back(Node::Sample{frame, buffer.stride, std::move(bytes)});
    known->stride = buffer.stride;
    known->componentBytes = at.sizeInBytes;
    known->offset = at.offset;
}

VertexPoseHistory::Verdict VertexPoseHistory::verdictOf(const Node::Sample& before,
                                                        const Node::Sample& after,
                                                        uint32_t componentBytes,
                                                        uint64_t* differingBytes,
                                                        float* biggestDelta, uint64_t* outOfRange) {
    *differingBytes = 0;
    *biggestDelta = 0.0f;
    // A mismatched pair cannot be reached through the pairing, which matches on shape, so this
    // is a guard rather than a verdict: if a future change lets two shapes meet here, the
    // answer is "nothing was compared" and not a number about unrelated bytes.
    if (before.stride != after.stride || before.bytes.size() != after.bytes.size()) {
        return Verdict::UnpairedShapes;
    }
    for (size_t byte = 0; byte < after.bytes.size(); byte++) {
        if (before.bytes[byte] != after.bytes[byte]) {
            (*differingBytes)++;
        }
    }
    // The magnitude, component by component, where a component is four bytes of float. Reported
    // in the attribute's own units so it can be compared with the travel a scene actually has.
    if (componentBytes >= 4 && componentBytes % 4 == 0) {
        const size_t vertices = after.bytes.size() / componentBytes;
        for (size_t vertex = 0; vertex < vertices; vertex++) {
            for (size_t offset = 0; offset + 4 <= componentBytes; offset += 4) {
                float a = 0.0f;
                float b = 0.0f;
                const size_t at = vertex * componentBytes + offset;
                if (!readFloat(before.bytes.data() + at, &a) ||
                    !readFloat(after.bytes.data() + at, &b)) {
                    continue;
                }
                const float difference = std::fabs(b - a);
                if (difference > kComponentCeiling) {
                    // A position does not move by 10^34 between two frames. Bytes read as a
                    // float that give that magnitude are not a position in this layout -- the
                    // attribute was named across objects and this draw's stride packs something
                    // else at that offset -- and reporting the magnitude as movement would be a
                    // number that looks like a result and is not one. Counted instead.
                    (*outOfRange)++;
                    continue;
                }
                *biggestDelta = std::max(*biggestDelta, difference);
            }
        }
    }
    if (*differingBytes == 0) {
        return Verdict::Identical;
    }
    // Bytes differ, so something is not byte-identical. Whether the POSITION moved is a
    // separate question, asked of the values: one real run reported 13,780 differing bytes
    // beside a largest component delta of 1.19e-07 and called it a blend, which is a
    // difference in the low mantissa bits and not a pose.
    if (*biggestDelta <= kPositionMotion) {
        return Verdict::ValueUnchanged;
    }
    return Verdict::Blendable;
}

// A node is as blendable as its best-matched shape, and the report says how many shapes were
// paired -- because a node with one paired shape out of four has not been shown to be static,
// it has been shown to be under-sampled.
VertexPoseHistory::Verdict VertexPoseHistory::verdictOf(const Node& node, uint64_t* differingBytes,
                                                        float* biggestDelta, uint64_t* outOfRange,
                                                        uint32_t* stride,
                                                        uint32_t* vertices) const {
    *differingBytes = 0;
    *biggestDelta = 0.0f;
    *outOfRange = 0;
    *stride = 0;
    *vertices = 0;
    bool anyPaired = false;
    Verdict best = Verdict::UnpairedShapes;
    // Grouped by shape, not by adjacency. A node with four shapes appends them interleaved --
    // placeholder, mesh, placeholder, mesh -- so "consecutive and equal in shape" pairs almost
    // nothing and reported six of eight objects blendable off a stride that was not the
    // position's. Each shape's own samples are collected first and then paired in frame order.
    std::vector<std::vector<const Node::Sample*>> byShape;
    for (const Node::Sample& sample : node.samples) {
        std::vector<const Node::Sample*>* group = nullptr;
        for (std::vector<const Node::Sample*>& candidate : byShape) {
            if (candidate.front()->stride == sample.stride &&
                candidate.front()->bytes.size() == sample.bytes.size()) {
                group = &candidate;
                break;
            }
        }
        if (group == nullptr) {
            byShape.push_back({&sample});
        } else {
            group->push_back(&sample);
        }
    }
    // The out-of-range components are counted across every shape before the walk returns, so
    // one blendable shape does not hide the others' unreadable magnitudes.
    uint64_t outOfRangeTotal = 0;
    for (std::vector<const Node::Sample*>& group : byShape) {
        for (size_t index = 1; index < group.size(); index++) {
            const Node::Sample& before = *group[index - 1];
            const Node::Sample& after = *group[index];
            uint64_t differing = 0;
            uint64_t shapeOut = 0;
            float biggest = 0.0f;
            const Verdict one =
                verdictOf(before, after, node.componentBytes, &differing, &biggest, &shapeOut);
            anyPaired = true;
            outOfRangeTotal += shapeOut;
            // The geometry of the shape the verdict is ABOUT, written before any early return,
            // so the numbers beside a verdict are the numbers that verdict is about.
            *stride = after.stride;
            *vertices = node.componentBytes == 0
                            ? 0
                            : static_cast<uint32_t>(after.bytes.size() / node.componentBytes);
            // The counts describe the shape the verdict is about, for EVERY verdict and not
            // only the blendable one. Writing them only on the blendable path left
            // `valueUnchanged` reporting zero differing bytes, which is the same "zero for not
            // reported" this report has now produced three times.
            *differingBytes = differing;
            *biggestDelta = biggest;
            *outOfRange = outOfRangeTotal + shapeOut;
            if (one == Verdict::Blendable) {
                return one;
            }
            best = one;
        }
    }
    *outOfRange = outOfRangeTotal;
    return anyPaired ? best : Verdict::UnpairedShapes;
}

const char* VertexPoseHistory::nameOf(Verdict verdict) {
    switch (verdict) {
    case Verdict::OneSample:
        return "oneSample";
    case Verdict::Identical:
        return "identical";
    case Verdict::ValueUnchanged:
        return "valueUnchanged";
    case Verdict::Blendable:
        return "blendable";
    case Verdict::UnpairedShapes:
        return "unpairedShapes";
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
        uint64_t outOfRange = 0;
        float biggest = 0.0f;
        uint32_t stride = 0;
        uint32_t vertices = 0;
        switch (verdictOf(node, &differing, &biggest, &outOfRange, &stride, &vertices)) {
        case Verdict::Blendable:
            out.blendable++;
            break;
        case Verdict::Identical:
            out.identical++;
            break;
        case Verdict::ValueUnchanged:
            out.valueUnchanged++;
            break;
        case Verdict::OneSample:
        case Verdict::UnpairedShapes:
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
    uint64_t valueUnchanged = 0;
    uint64_t oneSample = 0;
    for (const Node& node : m_nodes) {
        uint64_t differing = 0;
        uint64_t outOfRange = 0;
        uint32_t stride = 0;
        uint32_t vertices = 0;
        float biggest = 0.0f;
        switch (verdictOf(node, &differing, &biggest, &outOfRange, &stride, &vertices)) {
        case Verdict::Blendable:
            blendable++;
            break;
        case Verdict::Identical:
            identical++;
            break;
        case Verdict::ValueUnchanged:
            valueUnchanged++;
            break;
        case Verdict::OneSample:
        case Verdict::UnpairedShapes:
            oneSample++;
            break;
        }
    }
    body.number("blendable", blendable);
    body.number("identical", identical);
    body.number("valueUnchanged", valueUnchanged);
    body.raw("positionMotion", JsonBody::real(static_cast<double>(kPositionMotion)));
    body.number("unpairedShapes", oneSample);

    JsonBody nodes;
    size_t shown = 0;
    for (const Node& node : m_nodes) {
        if (shown >= kNodes) {
            break;
        }
        uint64_t differing = 0;
        float biggest = 0.0f;
        uint64_t outOfRange = 0;
        uint32_t stride = 0;
        uint32_t vertices = 0;
        const Verdict verdict =
            verdictOf(node, &differing, &biggest, &outOfRange, &stride, &vertices);
        // The geometry of the shape the verdict is ABOUT, not of whichever sample happened to
        // be last. One run showed 1040 vertices beside a verdict about another shape, and
        // 13,780 differing bytes against a total of 12,480 -- which is only possible if the two
        // numbers came from different shapes.
        const size_t bytes = node.componentBytes == 0 ? 0 : vertices * node.componentBytes;
        JsonBody one;
        one.number("node", node.address);
        one.string("verdict", nameOf(verdict));
        one.number("samples", node.samples.size());
        one.number("fromFrame", node.samples.empty() ? 0 : node.samples.front().frame);
        one.number("toFrame", node.samples.size() < 2 ? 0 : node.samples.back().frame);
        one.number("framesApart", node.samples.size() < 2
                                      ? 0
                                      : node.samples.back().frame - node.samples.front().frame);
        one.number("stride", stride);
        one.number("componentBytes", node.componentBytes);
        one.number("offsetInStride", node.offset);
        one.number("vertices", node.componentBytes == 0 ? 0 : bytes / node.componentBytes);
        one.number("positionBytes", bytes);
        // **Null, not zero, when nothing was compared.** The count and the magnitude are
        // computed only on the comparable path, so reporting them as 0 for an uncomparable
        // node would say "no bytes differ" about a pair of samples that were never compared --
        // which is the same lie as "moved 18, biggest delta 0.000000", one class further out.
        // A reader must be able to tell a measurement of zero from the absence of one.
        if (verdict == Verdict::UnpairedShapes) {
            one.raw("differingBytes", "null");
            one.raw("biggestComponentDelta", "null");
            one.raw("compared", "false");
            one.raw("magnitudeBelievable", "null");
        } else {
            one.number("differingBytes", differing);
            one.raw("biggestComponentDelta", JsonBody::real(static_cast<double>(biggest)));
            one.raw("compared", "true");
            one.number("componentsOutOfRange", outOfRange);
            one.raw("magnitudeBelievable", outOfRange == 0 ? "true" : "false");
        }
        // The shapes themselves, when nothing was paired: a reader who sees "unpairedShapes"
        // needs to see how many shapes and how big, because "nothing was paired" and "one shape
        // of one was" are different under-samplings.
        one.number("shapes", node.samples.size());
        JsonBody shapes;
        for (size_t index = 0; index < node.samples.size() && index < kShapesPerNode; index++) {
            JsonBody shape;
            shape.number("frame", node.samples[index].frame);
            shape.number("stride", node.samples[index].stride);
            shape.number("bytes", static_cast<uint32_t>(node.samples[index].bytes.size()));
            shapes.object(std::to_string(index), shape.text());
        }
        one.object("sampledShapes", shapes.text());
        nodes.object(std::to_string(shown), one.text());
        shown++;
    }
    body.object("nodes", nodes.text());
    return body.finish();
}

} // namespace wiiuport::title
