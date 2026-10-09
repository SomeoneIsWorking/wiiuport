#include "wiiuport/title/QuadBlend.h"

#include "wiiuport/interp/Blendable.h"
#include "wiiuport/title/JsonBody.h"
#include "wiiuport/title/VertexComponent.h"

#include <cstddef>
#include <optional>
#include <span>

namespace wiiuport::title {

bool QuadBlend::onDraw(const LatteFrameHooks::DrawPrepared& draw,
                       LatteFrameHooks::VertexReplacements& replacements) {
    Report report;
    bool replaced = false;
    const bool gated = m_gated();
    for (uint32_t index = 0; index < draw.vertexBufferCount; index++) {
        const LatteFrameHooks::DrawPrepared::VertexBuffer& buffer = draw.vertexBuffers[index];
        report.buffers++;
        const std::optional<guest::BufferWriters::Write> write = m_writers.written(
            buffer.data, std::span(static_cast<const std::byte*>(buffer.data), buffer.sizeInBytes));
        if (!write.has_value()) {
            continue;
        }
        report.written++;
        if (!gated) {
            report.ungated++;
            continue;
        }
        if (!write->before.has_value()) {
            report.firstSight++;
            continue;
        }
        if (*write->before == write->leading) {
            report.repeated++;
            continue;
        }
        if (blend(draw, index, *write->before, report)) {
            replacements.data[index] = m_copies[index].data();
            replaced = true;
        }
    }
    std::scoped_lock lock(m_mutex);
    m_report.buffers += report.buffers;
    m_report.written += report.written;
    m_report.ungated += report.ungated;
    m_report.firstSight += report.firstSight;
    m_report.repeated += report.repeated;
    m_report.withoutVertices += report.withoutVertices;
    m_report.longerThanWritten += report.longerThanWritten;
    m_report.withoutPosition += report.withoutPosition;
    m_report.ambiguousPosition += report.ambiguousPosition;
    m_report.refusedUnblendable += report.refusedUnblendable;
    m_report.blended += report.blended;
    m_report.moved += report.moved;
    return replaced;
}

bool QuadBlend::blend(const LatteFrameHooks::DrawPrepared& draw, uint32_t index,
                      const guest::BufferWriters::Leading& before, Report& report) {
    const LatteFrameHooks::DrawPrepared::VertexBuffer& buffer = draw.vertexBuffers[index];
    if (buffer.stride == 0 || buffer.vertices == 0) {
        report.withoutVertices++;
        return false;
    }
    const uint32_t vertices = buffer.vertices;
    // Only the bytes the writer was seen writing have a tick before to blend from.
    if (static_cast<size_t>(vertices - 1) * buffer.stride + kPositionBytes >
        guest::BufferWriters::kLeadingBytes) {
        report.longerThanWritten++;
        return false;
    }
    const LatteFrameHooks::DrawPrepared::VertexAttribute* position = nullptr;
    uint32_t candidates = 0;
    for (uint32_t at = 0; at < draw.vertexAttributeCount; at++) {
        const LatteFrameHooks::DrawPrepared::VertexAttribute& attribute = draw.vertexAttributes[at];
        if (attribute.buffer == index && attribute.format == kPositionFormat &&
            attribute.sizeInBytes == kPositionBytes && !attribute.perInstance) {
            position = &attribute;
            candidates++;
        }
    }
    if (candidates == 0) {
        report.withoutPosition++;
        return false;
    }
    if (candidates > 1) {
        report.ambiguousPosition++;
        return false;
    }
    const size_t lastByte =
        static_cast<size_t>(vertices - 1) * buffer.stride + position->offset + kPositionBytes;
    if (lastByte > guest::BufferWriters::kLeadingBytes || lastByte > buffer.sizeInBytes) {
        report.longerThanWritten++;
        return false;
    }
    const VertexComponent component(position->endianSwap);
    std::vector<float> from(static_cast<size_t>(vertices) * kComponents);
    std::vector<float> now(from.size());
    const auto* current = static_cast<const uint8_t*>(buffer.data);
    const auto* previous = reinterpret_cast<const uint8_t*>(before.data());
    for (uint32_t vertex = 0; vertex < vertices; vertex++) {
        for (uint32_t axis = 0; axis < kComponents; axis++) {
            const size_t at = static_cast<size_t>(vertex) * buffer.stride + position->offset +
                              static_cast<size_t>(axis) * sizeof(float);
            const size_t slot = static_cast<size_t>(vertex) * kComponents + axis;
            from[slot] = component.read(previous + at);
            now[slot] = component.read(current + at);
        }
    }
    std::vector<float> blended(from.size());
    if (!interp::midpoint(from, now, blended)) {
        report.refusedUnblendable++;
        return false;
    }
    std::vector<uint8_t>& copy = m_copies[index];
    copy.assign(current, current + buffer.sizeInBytes);
    for (uint32_t vertex = 0; vertex < vertices; vertex++) {
        for (uint32_t axis = 0; axis < kComponents; axis++) {
            const size_t at = static_cast<size_t>(vertex) * buffer.stride + position->offset +
                              static_cast<size_t>(axis) * sizeof(float);
            component.write(copy.data() + at,
                            blended[static_cast<size_t>(vertex) * kComponents + axis]);
        }
    }
    report.blended++;
    if (!interp::sameBits(from, now)) {
        report.moved++;
    }
    return true;
}

QuadBlend::Report QuadBlend::report() const {
    std::scoped_lock lock(m_mutex);
    return m_report;
}

std::string QuadBlend::json() const {
    const Report r = report();
    JsonBody body;
    body.number("writersInstalled", m_writers.installed());
    body.number("writes", m_writers.calls());
    body.number("unreadableWrites", m_writers.unreadable());
    body.number("identified", m_writers.identified());
    body.number("rewritten", m_writers.rewritten());
    body.number("buffers", r.buffers);
    body.number("written", r.written);
    body.number("ungated", r.ungated);
    body.number("firstSight", r.firstSight);
    body.number("repeated", r.repeated);
    body.number("withoutVertices", r.withoutVertices);
    body.number("longerThanWritten", r.longerThanWritten);
    body.number("withoutPosition", r.withoutPosition);
    body.number("ambiguousPosition", r.ambiguousPosition);
    body.number("refusedUnblendable", r.refusedUnblendable);
    body.number("blended", r.blended);
    body.number("moved", r.moved);
    return body.finish();
}

} // namespace wiiuport::title
