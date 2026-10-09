#pragma once

#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/guest/BufferWriters.h"

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wiiuport::title {

// The quads the title's CPU writes each paint -- particles, sea waves, sky clouds -- blended on the
// first paint of each tick.
//
// With the logic gate in, a tick spans two paints and the title writes the same quad on both. The
// write that changes an object's bytes is its first of a new tick, and the object's write before it
// (`BufferWriters`) holds the tick before's quad: that draw is given a copy of its buffer whose
// positions are the midpoint of the two. Paint parity does not say which write is first, because
// the gated tick can land between a paint's quad writes and its view upload.
class QuadBlend final : public frame::VertexReplacer {
  public:
    // Latte's 32_32_32_FLOAT: a quad's corner position.
    static constexpr uint8_t kPositionFormat = 0x30;
    static constexpr uint32_t kPositionBytes = 12;
    static constexpr uint32_t kComponents = 3;

    // Whether ticks are held to every other paint. Called on the Latte thread.
    using Gated = std::function<bool()>;

    QuadBlend(const guest::BufferWriters& writers, Gated gated)
        : m_writers(writers), m_gated(std::move(gated)) {
    }

    // Latte thread.
    bool onDraw(const LatteFrameHooks::DrawPrepared& draw,
                LatteFrameHooks::VertexReplacements& replacements) override;

    struct Report {
        uint64_t buffers = 0;
        // Buffers a writer was seen writing, with the bytes it wrote.
        uint64_t written = 0;
        // Written while ticks ran every paint.
        uint64_t ungated = 0;
        uint64_t firstSight = 0;
        // The tick's second write of the same bytes, drawn as written.
        uint64_t repeated = 0;
        uint64_t withoutVertices = 0;
        // Meshes longer than the bytes a writer is checked by: a 3D line's.
        uint64_t longerThanWritten = 0;
        // Buffers with no three-float position, and with more than one.
        uint64_t withoutPosition = 0;
        uint64_t ambiguousPosition = 0;
        uint64_t refusedUnblendable = 0;
        uint64_t blended = 0;
        // Blends whose positions differ: the object moved, not only its UVs.
        uint64_t moved = 0;
    };

    Report report() const;
    std::string json() const;

  private:
    // Replaces buffer `index`'s positions with their midpoint from `before`, or says why not.
    bool blend(const LatteFrameHooks::DrawPrepared& draw, uint32_t index,
               const guest::BufferWriters::Leading& before, Report& report);

    const guest::BufferWriters& m_writers;
    Gated m_gated;
    // Latte thread only: the copies handed to the renderer, valid until the next draw.
    std::array<std::vector<uint8_t>, LatteFrameHooks::DrawPrepared::kMaxVertexBuffers> m_copies;
    mutable std::mutex m_mutex;
    Report m_report;
};

} // namespace wiiuport::title
