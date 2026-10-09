// The CPU-written quads, blended on the first paint of each tick at the draw.
#include "check.h"
#include "suites.h"
#include "wiiuport/guest/BufferWriters.h"
#include "wiiuport/title/QuadBlend.h"
#include "wiiuport/title/VertexComponent.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace {

using wiiuport::guest::BufferWriters;
using wiiuport::interp::GuestObject;
using wiiuport::title::QuadBlend;
using wiiuport::title::VertexComponent;

constexpr uint32_t kStride = 20;
constexpr uint32_t kCorners = 4;
// What Latte reads for a four-vertex draw at this stride: past the last corner's UV.
constexpr uint32_t kReadBytes = 92;
constexpr uint32_t kUvOffset = 12;

using Buffer = std::array<uint8_t, kReadBytes>;

// A quad as the sea-wave writer leaves it: big-endian corners offset by `x`, then the UVs.
Buffer quad(float x) {
    Buffer bytes{};
    const VertexComponent big(VertexComponent::kSwapU32);
    for (uint32_t corner = 0; corner < kCorners; corner++) {
        uint8_t* at = bytes.data() + static_cast<size_t>(corner) * kStride;
        big.write(at, x + static_cast<float>(corner));
        big.write(at + 4, 2.0f);
        big.write(at + 8, 3.0f);
        big.write(at + kUvOffset, 0.25f);
        big.write(at + kUvOffset + 4, 0.75f);
    }
    return bytes;
}

BufferWriters::Leading leadingOf(const Buffer& bytes) {
    BufferWriters::Leading leading{};
    std::memcpy(leading.data(), bytes.data(), leading.size());
    return leading;
}

float positionX(const void* bytes, uint32_t corner) {
    return VertexComponent(VertexComponent::kSwapU32)
        .read(static_cast<const uint8_t*>(bytes) + static_cast<size_t>(corner) * kStride);
}

// The title's side: the gate, and one object writing into its buffers paint by paint.
class Title {
  public:
    static constexpr uint32_t kObject = 0x3b7a0040;

    bool gated = true;
    BufferWriters writers;
    std::array<Buffer, 3> buffers{};
    // A sea wave has no age; a particle's grows by a tick.
    std::optional<float> age;

    // The object writes its quad at `x` into buffer `slot`, and returns what it wrote.
    const Buffer& paint(size_t slot, float x) {
        buffers.at(slot) = quad(x);
        writers.record(&buffers.at(slot), GuestObject{kObject, age}, leadingOf(buffers.at(slot)));
        return buffers.at(slot);
    }

    QuadBlend blend() {
        return QuadBlend{writers, [this] {
                             return gated;
                         }};
    }
};

LatteFrameHooks::DrawPrepared drawOf(const Buffer& bytes, uint32_t vertices = kCorners) {
    LatteFrameHooks::DrawPrepared draw{};
    draw.vertexBuffers[0] = {bytes.data(), kReadBytes, kStride, 0, vertices};
    draw.vertexBufferCount = 1;
    draw.vertexAttributes[0] = {
        0, 0,    QuadBlend::kPositionBytes, QuadBlend::kPositionFormat, VertexComponent::kSwapU32,
        0, false};
    draw.vertexAttributes[1] = {0, kUvOffset, 8, 0x1d, VertexComponent::kSwapU32, 1, false};
    draw.vertexAttributeCount = 2;
    return draw;
}

void aTicksFirstWriteDrawsTheMidpointOfTwoTicks() {
    Title title;
    title.paint(0, 10.0f);
    const Buffer& first = title.paint(1, 20.0f);
    QuadBlend blend = title.blend();
    LatteFrameHooks::VertexReplacements replacements;
    check::isTrue(blend.onDraw(drawOf(first), replacements), "the draw is replaced");
    const void* replaced = replacements.data[0];
    check::isTrue(replaced != nullptr, "by a copy of its buffer");
    if (replaced == nullptr) {
        return;
    }
    check::near(positionX(replaced, 0), 15.0f, 0.0f, "whose corners are the ticks' midpoint");
    check::near(positionX(replaced, 3), 18.0f, 0.0f, "every corner, to the last");
    const auto* copy = static_cast<const uint8_t*>(replaced);
    check::isTrue(std::memcmp(copy + kUvOffset, first.data() + kUvOffset, 8) == 0 &&
                      std::memcmp(copy + 80, first.data() + 80, kReadBytes - 80) == 0,
                  "and every other byte is the title's");
    check::near(positionX(first.data(), 0), 20.0f, 0.0f, "the guest's own buffer is not written");
    const QuadBlend::Report r = blend.report();
    check::isTrue(r.written == 1 && r.blended == 1 && r.moved == 1, "one blend, and it moved");
}

// Which paint of the pair carries the new tick is not fixed, so the repeat is found by its bytes.
void aTicksSecondWriteIsDrawnAsWritten() {
    Title title;
    title.paint(0, 10.0f);
    title.paint(1, 20.0f);
    const Buffer& second = title.paint(0, 20.0f);
    QuadBlend blend = title.blend();
    LatteFrameHooks::VertexReplacements replacements;
    check::isTrue(!blend.onDraw(drawOf(second), replacements) && replacements.data[0] == nullptr,
                  "the tick's quad written again is not replaced");
    check::isTrue(blend.report().repeated == 1, "and counted as a repeat");
}

void aParticlesSecondWriteIsARepeatToo() {
    Title title;
    title.age = 1.0f;
    title.paint(0, 10.0f);
    title.age = 2.0f;
    const Buffer& first = title.paint(1, 20.0f);
    QuadBlend blend = title.blend();
    LatteFrameHooks::VertexReplacements replacements;
    check::isTrue(blend.onDraw(drawOf(first), replacements), "a particle a tick older is blended");
    const Buffer& second = title.paint(0, 20.0f);
    check::isTrue(!blend.onDraw(drawOf(second), replacements),
                  "and its write at the same age is the same tick's");
    check::isTrue(blend.report().repeated == 1, "counted as a repeat, not a new particle");
}

void ungatedTicksAreDrawnAsWritten() {
    Title title;
    title.gated = false;
    title.paint(0, 10.0f);
    const Buffer& next = title.paint(1, 20.0f);
    QuadBlend blend = title.blend();
    LatteFrameHooks::VertexReplacements replacements;
    check::isTrue(!blend.onDraw(drawOf(next), replacements),
                  "with a tick every paint there is no paint between ticks to blend");
    check::isTrue(blend.report().ungated == 1, "and counted");
}

void aNewObjectHasNothingToBlendFrom() {
    Title title;
    const Buffer& first = title.paint(0, 20.0f);
    QuadBlend blend = title.blend();
    LatteFrameHooks::VertexReplacements replacements;
    check::isTrue(!blend.onDraw(drawOf(first), replacements),
                  "a quad with no write before it is left alone");
    check::isTrue(blend.report().firstSight == 1, "and counted as first sight");
}

void aBufferNoWriterWroteIsLeftAlone() {
    Title title;
    const Buffer model = quad(5.0f);
    QuadBlend blend = title.blend();
    LatteFrameHooks::VertexReplacements replacements;
    check::isTrue(!blend.onDraw(drawOf(model), replacements), "a model's buffer is not a quad's");
    const QuadBlend::Report r = blend.report();
    check::isTrue(r.buffers == 1 && r.written == 0, "and counted as a buffer only");
}

// Latte reads past the last corner, so its read size would make a fifth vertex of the UVs.
void theDrawsVertexCountBoundsTheBlend() {
    Title title;
    title.paint(0, 10.0f);
    const Buffer& first = title.paint(1, 20.0f);
    QuadBlend blend = title.blend();
    LatteFrameHooks::VertexReplacements replacements;
    check::isTrue(!blend.onDraw(drawOf(first, kCorners + 1), replacements),
                  "positions past the written bytes are not blended");
    check::isTrue(blend.report().longerThanWritten == 1, "and counted");
    check::isTrue(!blend.onDraw(drawOf(first, 0), replacements),
                  "an instance-only buffer has no positions to blend");
    check::isTrue(blend.report().withoutVertices == 1, "and is counted as such");
}

void aBufferWithoutAPositionIsLeftAlone() {
    Title title;
    title.paint(0, 10.0f);
    const Buffer& first = title.paint(1, 20.0f);
    QuadBlend blend = title.blend();
    LatteFrameHooks::DrawPrepared draw = drawOf(first);
    draw.vertexAttributes[0].format = 0x1d;
    draw.vertexAttributes[0].sizeInBytes = 8;
    LatteFrameHooks::VertexReplacements replacements;
    check::isTrue(!blend.onDraw(draw, replacements), "no three-float position, no blend");
    check::isTrue(blend.report().withoutPosition == 1, "and counted");
}

void twoPositionsAreNotGuessedBetween() {
    Title title;
    title.paint(0, 10.0f);
    const Buffer& first = title.paint(1, 20.0f);
    QuadBlend blend = title.blend();
    LatteFrameHooks::DrawPrepared draw = drawOf(first);
    draw.vertexAttributes[1].format = QuadBlend::kPositionFormat;
    draw.vertexAttributes[1].sizeInBytes = QuadBlend::kPositionBytes;
    LatteFrameHooks::VertexReplacements replacements;
    check::isTrue(!blend.onDraw(draw, replacements), "two three-float attributes, no blend");
    check::isTrue(blend.report().ambiguousPosition == 1, "and counted");
}

void anUnblendableQuadIsLeftAlone() {
    Title title;
    title.paint(0, std::numeric_limits<float>::quiet_NaN());
    const Buffer& first = title.paint(1, 20.0f);
    QuadBlend blend = title.blend();
    LatteFrameHooks::VertexReplacements replacements;
    check::isTrue(!blend.onDraw(drawOf(first), replacements),
                  "a write before with a NaN is not blended from");
    check::isTrue(blend.report().refusedUnblendable == 1, "and the refusal is counted");
}

} // namespace

void wiiuport::tests::runQuadBlendTests() {
    aTicksFirstWriteDrawsTheMidpointOfTwoTicks();
    aTicksSecondWriteIsDrawnAsWritten();
    aParticlesSecondWriteIsARepeatToo();
    ungatedTicksAreDrawnAsWritten();
    aNewObjectHasNothingToBlendFrom();
    aBufferNoWriterWroteIsLeftAlone();
    theDrawsVertexCountBoundsTheBlend();
    aBufferWithoutAPositionIsLeftAlone();
    twoPositionsAreNotGuessedBetween();
    anUnblendableQuadIsLeftAlone();
}
