#include "check.h"
#include "suites.h"
#include "vertex_blend_fixture.h"
#include "wiiuport/guest/RippleParticles.h"
#include "wiiuport/interp/DrawObjects.h"
#include "wiiuport/interp/VertexBlend.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using wiiuport::guest::RippleParticles;
using wiiuport::interp::GuestObject;
using wiiuport::interp::VertexOutcome;

namespace {

using namespace wiiuport::tests::vertex_blend;

constexpr size_t kValuesPerCorner = RippleParticles::kCornerStride / sizeof(float);

// A ring's quad as the title writes it about (x, z): each corner the centre
// plus one offset, the opposite corner the centre plus its negation. The
// offsets are the half-extents turned by the particle's angle, as its code
// turns them: `turned` and `across`, sums of two products.
std::vector<float> quadAbout(float x, float z, float turned, float across) {
    std::array<float, 4> dx{-turned, turned, turned, -turned};
    std::array<float, 4> dz{-across, -across, across, across};
    std::vector<float> values;
    for (size_t corner = 0; corner < RippleParticles::kCorners; ++corner) {
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

void aQuadTheTitleWroteIsCentredOnItsParticle() {
    // Corners whose rounding does not cancel: the two opposite corners sum
    // to other than twice the centre.
    float x = 123.456f;
    float z = -93847.61f;
    float turned = 6.5078f;
    float across = 3.0f;
    check::isTrue((x - turned) + (x + turned) != 2.0f * x,
                  "the case is one where rounding shows in the sum");
    std::vector<std::byte> bytes = bytesOfQuad(quadAbout(x, z, turned, across));
    check::isTrue(RippleParticles::centredOn(bytes, x, z),
                  "every corner rounded once still centres on the particle");
    float nextX = std::nextafter(x, std::numeric_limits<float>::infinity());
    float farX = std::nextafter(nextX, std::numeric_limits<float>::infinity());
    check::isTrue(!RippleParticles::centredOn(bytes, farX, z),
                  "not on a particle two float steps along");
    check::isTrue(!RippleParticles::centredOn(bytes, x, z + 1.0f), "nor on one beside it");
    check::isTrue(!RippleParticles::centredOn(std::span(bytes).first(bytes.size() - 1), x, z),
                  "and bytes short of a quad are no quad");
}

void aDrawIsTheParticleSeenDrawingItsBufferWhenItsVerticesSaySo() {
    RippleParticles particles;
    std::vector<std::byte> bytes = bytesOfQuad(quadAbout(40.0f, 8.0f, 3.0f, 2.0f));
    const void* source = bytes.data();
    check::isTrue(!particles.objectDrawn(source, bytes).has_value(),
                  "a buffer no particle was seen drawing names none");

    particles.record(source,
                     {.object = {.address = 0x4000'1000, .age = 7.0f}, .x = 40.0f, .z = 8.0f});
    std::optional<GuestObject> object = particles.objectDrawn(source, bytes);
    check::isTrue(object.has_value(), "the particle seen drawing it names it");
    check::equal(object.value_or(GuestObject{}).address, uint32_t{0x4000'1000}, "by its address");
    check::equal(object.value_or(GuestObject{}).age, 7.0f, "and its age");

    // The buffer handed to another particle that has not drawn yet: the
    // bytes are still the first particle's quad.
    particles.record(source,
                     {.object = {.address = 0x4000'2000, .age = 1.0f}, .x = 90.0f, .z = 8.0f});
    check::isTrue(!particles.objectDrawn(source, bytes).has_value(),
                  "a particle whose quad the bytes are not names nothing");
    check::equal(particles.calls(), uint64_t{2}, "two draw calls were recorded");
    check::equal(particles.identified(), uint64_t{1}, "one draw was identified");
    check::equal(particles.offCentre(), uint64_t{1}, "and one refused, counted");
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
        blends.ripples.record(
            kept.buffers.at(ripple.buffer).data(),
            {.object = {.address = ripple.address, .age = ripple.age}, .x = ripple.x, .z = 0.0f});
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

void runRippleParticlesTests() {
    aQuadTheTitleWroteIsCentredOnItsParticle();
    aDrawIsTheParticleSeenDrawingItsBufferWhenItsVerticesSaySo();
    aRingIsBlendedFromItsOwnParticleWhateverBuffersItIsGiven();
    aRingRebornInItsParticleIsDrawnAsTheTitleDrewIt();
    aRingWhoseParticleWasNotDrawnAFrameBeforeIsDrawnAsTheTitleDrewIt();
}

} // namespace wiiuport::tests
