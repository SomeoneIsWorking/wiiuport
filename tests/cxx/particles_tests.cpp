#include "check.h"
#include "suites.h"
#include "vertex_blend_fixture.h"
#include "wiiuport/guest/Particles.h"
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

using wiiuport::guest::Particles;
using wiiuport::interp::GuestObject;
using wiiuport::interp::VertexOutcome;

namespace {

using namespace wiiuport::tests::vertex_blend;

constexpr size_t kValuesPerCorner = 5;

// A particle's quad about (x, z): each corner the centre plus one offset, and
// the UVs every quad has.
std::vector<float> quadAbout(float x, float z, float turned, float across) {
    std::array<float, 4> dx{-turned, turned, turned, -turned};
    std::array<float, 4> dz{-across, -across, across, across};
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

Particles::Quad quadOf(std::span<const std::byte> bytes) {
    Particles::Quad quad{};
    std::ranges::copy(bytes.first(quad.size()), quad.begin());
    return quad;
}

void aDrawIsTheParticleThatWroteItsBufferWhileItReadsWhatWasWritten() {
    Particles particles;
    // A draw's buffer runs on past the quad, as the title's do.
    std::vector<std::byte> bytes = bytesOfQuad(quadAbout(40.0f, 8.0f, 3.0f, 2.0f));
    bytes.resize(bytes.size() + 12);
    const void* source = bytes.data();
    check::isTrue(!particles.objectDrawn(source, bytes).has_value(),
                  "a buffer no particle was seen writing names none");

    particles.record(source, {.address = 0x4000'1000, .age = 7.0f}, quadOf(bytes));
    std::optional<GuestObject> object = particles.objectDrawn(source, bytes);
    check::isTrue(object.has_value(), "the particle that wrote it names it");
    check::equal(object.value_or(GuestObject{}).address, uint32_t{0x4000'1000}, "by its address");
    check::equal(object.value_or(GuestObject{}).age, 7.0f, "and its age");

    // Another draw since wrote the buffer: one bit of the quad differs.
    std::vector<std::byte> since = bytes;
    since[Particles::kQuadBytes - 1] ^= std::byte{1};
    check::isTrue(!particles.objectDrawn(source, since).has_value(),
                  "bytes the particle did not write name nothing");
    check::isTrue(!particles.objectDrawn(source, std::span(bytes).first(Particles::kQuadBytes - 1))
                       .has_value(),
                  "nor do bytes short of a quad");
    check::equal(particles.calls(), uint64_t{1}, "one commit was recorded");
    check::equal(particles.identified(), uint64_t{1}, "one draw was identified");
    check::equal(particles.rewritten(), uint64_t{2}, "and two refused, counted");
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
        draws.push_back({.block = block,
                         .uniforms = {1.0f},
                         .mesh = quadAbout(ripple.x, 0.0f, ripple.radius, ripple.radius),
                         .valuesPerVertex = kValuesPerCorner,
                         .keptIn = ripple.buffer});
    }
    GuestFrame frame(std::move(draws), &kept);
    for (const Ripple& ripple : drawn) {
        const std::vector<std::byte>& buffer = kept.buffers.at(ripple.buffer);
        blends.particles.record(buffer.data(), {.address = ripple.address, .age = ripple.age},
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

} // namespace

namespace wiiuport::tests {

void runParticlesTests() {
    aDrawIsTheParticleThatWroteItsBufferWhileItReadsWhatWasWritten();
    aRingIsBlendedFromItsOwnParticleWhateverBuffersItIsGiven();
    aRingRebornInItsParticleIsDrawnAsTheTitleDrewIt();
    aRingWhoseParticleWasNotDrawnAFrameBeforeIsDrawnAsTheTitleDrewIt();
}

} // namespace wiiuport::tests
