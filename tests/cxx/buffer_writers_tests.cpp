#include "check.h"
#include "suites.h"
#include "vertex_blend_fixture.h"
#include "wiiuport/guest/BufferWriters.h"
#include "wiiuport/interp/DrawObjects.h"
#include "wiiuport/interp/VertexBlend.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

using wiiuport::guest::BufferWriters;
using wiiuport::interp::GuestObject;
using wiiuport::interp::VertexOutcome;

namespace {

using namespace wiiuport::tests::vertex_blend;

constexpr size_t kValuesPerCorner = 5;

// A quad on the water: its centre, and its half extents along x and z.
struct QuadShape {
    float x;
    float z;
    float halfX;
    float halfZ;
};

// The quad's four corners, each the centre plus one offset, with the UVs
// every quad has.
std::vector<float> quadAbout(const QuadShape& shape) {
    float x = shape.x;
    float z = shape.z;
    std::array<float, 4> dx{-shape.halfX, shape.halfX, shape.halfX, -shape.halfX};
    std::array<float, 4> dz{-shape.halfZ, -shape.halfZ, shape.halfZ, shape.halfZ};
    std::vector<float> values;
    for (size_t corner = 0; corner < 4; ++corner) {
        std::array<float, kValuesPerCorner> vertex{x + dx[corner], 0.0f, z + dz[corner],
                                                   corner == 1 || corner == 2 ? 1.0f : 0.0f,
                                                   corner >= 2 ? 1.0f : 0.0f};
        values.insert(values.end(), vertex.begin(), vertex.end());
    }
    return values;
}

std::vector<std::byte> bytesOfQuad(const std::vector<float>& values) {
    std::vector<std::byte> bytes(values.size() * sizeof(float));
    for (size_t index = 0; index < values.size(); ++index) {
        putWord(kBigEndian, bytes.data() + (index * sizeof(float)),
                std::bit_cast<uint32_t>(values[index]));
    }
    return bytes;
}

BufferWriters::Leading quadOf(std::span<const std::byte> bytes) {
    BufferWriters::Leading quad{};
    std::ranges::copy(bytes.first(quad.size()), quad.begin());
    return quad;
}

void aDrawIsTheParticleThatWroteItsBufferWhileItReadsWhatWasWritten() {
    BufferWriters writers;
    // A draw's buffer runs on past the quad, as the title's do.
    std::vector<std::byte> bytes =
        bytesOfQuad(quadAbout({.x = 40.0f, .z = 8.0f, .halfX = 3.0f, .halfZ = 2.0f}));
    bytes.resize(bytes.size() + 12);
    const void* source = bytes.data();
    check::isTrue(!writers.objectDrawn(source, bytes).has_value(),
                  "a buffer no particle was seen writing names none");

    writers.record(source, {.address = 0x4000'1000, .age = 7.0f}, quadOf(bytes));
    std::optional<GuestObject> object = writers.objectDrawn(source, bytes);
    check::isTrue(object.has_value(), "the particle that wrote it names it");
    check::equal(object.value_or(GuestObject{}).address, uint32_t{0x4000'1000}, "by its address");
    check::equal(object.value_or(GuestObject{}).age.value_or(0.0f), 7.0f, "and its age");

    // Another draw since wrote the buffer: one bit of the quad differs.
    std::vector<std::byte> since = bytes;
    since[BufferWriters::kLeadingBytes - 1] ^= std::byte{1};
    check::isTrue(!writers.objectDrawn(source, since).has_value(),
                  "bytes the particle did not write name nothing");
    check::isTrue(
        !writers.objectDrawn(source, std::span(bytes).first(BufferWriters::kLeadingBytes - 1))
             .has_value(),
        "nor do bytes short of a quad");
    check::equal(writers.calls(), uint64_t{1}, "one commit was recorded");
    check::equal(writers.identified(), uint64_t{1}, "one draw was identified");
    check::equal(writers.rewritten(), uint64_t{2}, "and two refused, counted");
}

// A ripple particle drawn in a frame: its address, age and place, and the
// kept buffer the title gave it.
struct Ripple {
    uint32_t address;
    float age;
    float x;
    float radius;
    size_t buffer;
};

// A frame of ripple rings, every one drawn with the same blocks -- told
// apart by nothing but their place -- each recorded as its particle's.
GuestFrame ripples(Blends& blends, KeptBuffers& kept, uint32_t block,
                   const std::vector<Ripple>& drawn) {
    std::vector<ActorDraw> draws;
    draws.reserve(drawn.size());
    for (const Ripple& ripple : drawn) {
        draws.push_back(
            {.block = block,
             .uniforms = {1.0f},
             .mesh = quadAbout(
                 {.x = ripple.x, .z = 0.0f, .halfX = ripple.radius, .halfZ = ripple.radius}),
             .valuesPerVertex = kValuesPerCorner,
             .keptIn = ripple.buffer});
    }
    GuestFrame frame(std::move(draws), &kept);
    for (const Ripple& ripple : drawn) {
        const std::vector<std::byte>& buffer = kept.buffers.at(ripple.buffer);
        blends.writers.record(buffer.data(), {.address = ripple.address, .age = ripple.age},
                              quadOf(buffer));
    }
    return frame;
}

constexpr uint32_t kRippleBlock = 0xf4006000;
constexpr uint32_t kRippleBlockB = 0xf4086000;
constexpr uint32_t kFirst = 0x4000'1000;
constexpr uint32_t kSecond = 0x4000'2000;

void aRingIsBlendedFromItsOwnParticleWhateverBuffersItIsGiven() {
    // Two rings growing, drawn in either order, told apart by nothing in
    // their draws. The first is given buffers 0, 1 and then 2: never the
    // buffer it drew from two frames back.
    Blends blends;
    blends.objects.setPlanning(true);
    KeptBuffers kept;
    blends.record(ripples(blends, kept, kRippleBlock,
                          {{kFirst, 1.0f, 100.0f, 1.0f, 0}, {kSecond, 4.0f, 300.0f, 9.0f, 4}}));
    blends.record(ripples(blends, kept, kRippleBlockB,
                          {{kSecond, 5.0f, 300.0f, 10.0f, 5}, {kFirst, 2.0f, 100.0f, 3.0f, 1}}));
    GuestFrame latest =
        ripples(blends, kept, kRippleBlock,
                {{kFirst, 3.0f, 100.0f, 5.0f, 2}, {kSecond, 6.0f, 300.0f, 11.0f, 4}});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 96.0f,
                 "the first ring's corner is half way from its own a frame before");
    check::equal(drawn[1][0], 289.5f, "and so is the second's");
    check::equal(blends.vertices.draws(VertexOutcome::Blended), uint64_t{2},
                 "both blended from their particles");
    check::equal(blends.vertices.objectDraws(VertexOutcome::Blended), uint64_t{2},
                 "and counted among the title's objects");
}

void aRingRebornInItsParticleIsDrawnAsTheTitleDrewIt() {
    // A particle the pool gave a new ring at N: younger than the ring it drew
    // a frame before, in the buffer it drew two frames back.
    Blends blends;
    blends.objects.setPlanning(true);
    KeptBuffers kept;
    blends.record(ripples(blends, kept, kRippleBlock, {{kFirst, 20.0f, 100.0f, 20.0f, 0}}));
    blends.record(ripples(blends, kept, kRippleBlockB, {{kFirst, 21.0f, 100.0f, 21.0f, 1}}));
    GuestFrame latest = ripples(blends, kept, kRippleBlock, {{kFirst, 0.0f, 100.0f, 22.0f, 0}});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 78.0f, "the new ring is drawn as the title drew it");
    check::equal(blends.vertices.objectDraws(VertexOutcome::NoPartner), uint64_t{1},
                 "counted with no partner among the title's objects");
}

void aRingWhoseParticleWasNotDrawnAFrameBeforeIsDrawnAsTheTitleDrewIt() {
    Blends blends;
    blends.objects.setPlanning(true);
    KeptBuffers kept;
    blends.record(ripples(blends, kept, kRippleBlock, {{kFirst, 1.0f, 100.0f, 1.0f, 0}}));
    blends.record(ripples(blends, kept, kRippleBlockB, {{kSecond, 1.0f, 100.0f, 2.0f, 1}}));
    GuestFrame latest = ripples(blends, kept, kRippleBlock, {{kFirst, 3.0f, 100.0f, 3.0f, 0}});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 97.0f, "a ring its particle did not draw a frame before is N's");
    check::equal(blends.vertices.draws(VertexOutcome::NoPartner), uint64_t{1},
                 "counted with no partner");
}

void anObjectContinuesAsItselfOnlyIfItsAgeSaysSo() {
    GuestObject aged{.address = kFirst, .age = 3.0f};
    GuestObject line{.address = kFirst, .age = std::nullopt};
    check::isTrue(aged.continuesAs({.address = kFirst, .age = 4.0f}),
                  "an object older by N continues");
    check::isTrue(!aged.continuesAs({.address = kFirst, .age = 3.0f}),
                  "one no older is reborn at its address");
    check::isTrue(!aged.continuesAs({.address = kSecond, .age = 4.0f}),
                  "one at another address is another object");
    check::isTrue(line.continuesAs(line), "an object without an age continues while drawn");
    check::isTrue(!line.continuesAs(aged) && !aged.continuesAs(line),
                  "an aged and an unaged object are not one");
}

constexpr uint64_t kShadowShader = 0xeeee;
constexpr uint32_t kLineBlock = 0xf4007000;
constexpr uint32_t kShadowBlock = 0xf4008000;

// A 3D line at `x`, its mesh in the buffer it owns and recorded as its own,
// drawn by its shader and then read again by a shadow volume's.
GuestFrame lineAt(Blends& blends, KeptBuffers& kept, float x) {
    GuestFrame frame({{.block = kLineBlock,
                       .uniforms = {1.0f},
                       .mesh = quadAbout({.x = x, .z = 0.0f, .halfX = 2.0f, .halfZ = 2.0f}),
                       .valuesPerVertex = kValuesPerCorner,
                       .keptIn = 0},
                      {.block = kShadowBlock,
                       .uniforms = {1.0f},
                       .mesh = quadAbout({.x = x, .z = 0.0f, .halfX = 2.0f, .halfZ = 2.0f}),
                       .passOver = 0,
                       .valuesPerVertex = kValuesPerCorner,
                       .shader = kShadowShader}},
                     &kept);
    const std::vector<std::byte>& buffer = kept.buffers.at(0);
    blends.writers.record(buffer.data(), {.address = kFirst, .age = std::nullopt}, quadOf(buffer));
    return frame;
}

void aLineWithoutAnAgeIsBlendedFromItsOwnDrawAndItsShadowWithIt() {
    Blends blends;
    blends.objects.setPlanning(true);
    KeptBuffers kept;
    blends.record(lineAt(blends, kept, 100.0f));
    blends.record(lineAt(blends, kept, 102.0f));
    GuestFrame latest = lineAt(blends, kept, 104.0f);
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 101.0f, "the line is half way from its draw a frame before");
    check::equal(drawn[1][0], 101.0f, "and its shadow, reading the same buffer, with it");
    check::equal(blends.vertices.objectDraws(VertexOutcome::Blended), uint64_t{2},
                 "counted as the title's line");
}

} // namespace

namespace wiiuport::tests {

void runBufferWritersTests() {
    aDrawIsTheParticleThatWroteItsBufferWhileItReadsWhatWasWritten();
    aRingIsBlendedFromItsOwnParticleWhateverBuffersItIsGiven();
    aRingRebornInItsParticleIsDrawnAsTheTitleDrewIt();
    aRingWhoseParticleWasNotDrawnAFrameBeforeIsDrawnAsTheTitleDrewIt();
    anObjectContinuesAsItselfOnlyIfItsAgeSaysSo();
    aLineWithoutAnAgeIsBlendedFromItsOwnDrawAndItsShadowWithIt();
}

} // namespace wiiuport::tests
