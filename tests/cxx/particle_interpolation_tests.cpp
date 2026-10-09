// The paint's particles: each drawn at the midpoint of its last two ticks in the in-between paint.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/ParticleInterpolation.h"

#include <bit>
#include <cstdint>
#include <map>
#include <vector>

namespace {

using wiiuport::title::ParticleInterpolation;

constexpr uint32_t kDraw = 0x3a000000;
constexpr uint32_t kEmitter = 0x3b000000;
constexpr uint32_t kFirstLink = 0x3c000000;
constexpr uint32_t kFirstParticle = 0x3d000000;
constexpr uint32_t kChildLink = 0x3c100000;
constexpr uint32_t kChild = 0x3d100000;

void noRegistration(uint32_t /*entry*/, uint32_t /*firstInstruction*/,
                    GuestCallProbes::Probe& /*probe*/, bool /*holdsEntry*/, uint32_t /*resume*/) {
}

uint32_t word(float value) {
    return std::bit_cast<uint32_t>(value);
}

float value(uint32_t word) {
    return std::bit_cast<float>(word);
}

struct Particle {
    float x;
    float age;
};

class Guest {
  public:
    std::map<uint32_t, uint32_t> words;

    ParticleInterpolation interpolation() {
        return ParticleInterpolation{
            {.registerProbe = &noRegistration,
             .readWords =
                 [this](uint32_t address, uint32_t* out, uint32_t count) {
                     for (uint32_t i = 0; i < count; ++i) {
                         out[i] = words[address + (4 * i)];
                     }
                     return true;
                 },
             .writeWords =
                 [this](uint32_t address, const uint32_t* in, uint32_t count) {
                     for (uint32_t i = 0; i < count; ++i) {
                         words[address + (4 * i)] = in[i];
                     }
                     return true;
                 }}};
    }

    // The emitter's active list, one link per particle, and one child.
    void emitter(const std::vector<Particle>& particles, Particle child) {
        words[kDraw + ParticleInterpolation::kDrawEmitter] = kEmitter;
        words[kEmitter + ParticleInterpolation::kParticleLists[0]] = kFirstLink;
        for (uint32_t i = 0; i < particles.size(); ++i) {
            uint32_t link = kFirstLink + (0x10 * i);
            uint32_t particle = kFirstParticle + (0x100 * i);
            words[link + ParticleInterpolation::kLinkObject] = particle;
            words[link + ParticleInterpolation::kLinkNext] =
                i + 1 < particles.size() ? link + 0x10 : 0;
            place(particle, particles[i]);
        }
        words[kEmitter + ParticleInterpolation::kParticleLists[1]] = kChildLink;
        words[kChildLink + ParticleInterpolation::kLinkObject] = kChild;
        words[kChildLink + ParticleInterpolation::kLinkNext] = 0;
        place(kChild, child);
    }

    void place(uint32_t particle, Particle at) {
        words[particle + ParticleInterpolation::kGlobalPosition] = word(at.x);
        words[particle + ParticleInterpolation::kGlobalPosition + 4] = word(2.0f);
        words[particle + ParticleInterpolation::kGlobalPosition + 8] = word(3.0f);
        words[particle + ParticleInterpolation::kAge] = word(at.age);
    }

    float x(uint32_t particle) {
        return value(words[particle + ParticleInterpolation::kGlobalPosition]);
    }
};

void aParticleIsPaintedAtItsMidpointAndPutBack() {
    Guest guest;
    ParticleInterpolation particles = guest.interpolation();
    guest.emitter({{.x = 0.0f, .age = 3.0f}}, {.x = 100.0f, .age = 1.0f});
    particles.onEmitterDraw(kDraw);
    guest.emitter({{.x = 10.0f, .age = 4.0f}}, {.x = 120.0f, .age = 2.0f});
    particles.onMidPaintBegin(1);
    particles.onEmitterDraw(kDraw);
    check::isTrue(guest.x(kFirstParticle) == 5.0f,
                  "the in-between paint draws a particle between the last paint and now");
    check::isTrue(value(guest.words[kFirstParticle + ParticleInterpolation::kGlobalPosition + 4]) ==
                      2.0f,
                  "on every axis, an unmoved one where it is");
    check::isTrue(guest.x(kChild) == 110.0f, "and the children too");
    particles.onEmitterDraw(kDraw);
    check::isTrue(guest.x(kFirstParticle) == 5.0f,
                  "a second draw of the emitter in the same paint does not blend it again");
    particles.onMidPaintEnd();
    check::isTrue(guest.x(kFirstParticle) == 10.0f && guest.x(kChild) == 120.0f,
                  "and the next draw phase sees the tick's own positions");
}

void aRenewedSlotIsPaintedAsItIs() {
    Guest guest;
    ParticleInterpolation particles = guest.interpolation();
    guest.emitter({{.x = 0.0f, .age = 30.0f}}, {.x = 0.0f, .age = 1.0f});
    particles.onEmitterDraw(kDraw);
    // The pool gave the slot to a new particle, born somewhere else.
    guest.emitter({{.x = 50.0f, .age = 0.0f}}, {.x = 0.0f, .age = 2.0f});
    particles.onMidPaintBegin(1);
    particles.onEmitterDraw(kDraw);
    check::isTrue(guest.x(kFirstParticle) == 50.0f,
                  "a particle younger than the one last painted there is a new one, not a move");
    particles.onMidPaintEnd();
}

void onlyTheLastTicksPaintIsAnEnd() {
    Guest guest;
    ParticleInterpolation particles = guest.interpolation();
    guest.emitter({{.x = 0.0f, .age = 1.0f}}, {.x = 0.0f, .age = 1.0f});
    particles.onEmitterDraw(kDraw);
    guest.emitter({{.x = 10.0f, .age = 2.0f}}, {.x = 0.0f, .age = 2.0f});
    particles.onMidPaintBegin(2);
    particles.onEmitterDraw(kDraw);
    check::isTrue(guest.x(kFirstParticle) == 10.0f,
                  "a paint from two ticks back is not the other end of this tick's move");
    particles.onMidPaintEnd();
    particles.onMidPaintBegin(3);
    particles.onEmitterDraw(kDraw);
    check::isTrue(guest.x(kFirstParticle) == 10.0f,
                  "and nothing is recorded from an in-between paint");
    particles.onMidPaintEnd();
}

// A particle's draw parameters, as its emitter's calc leaves them.
struct DrawParams {
    float scale;
    uint32_t color;
    int16_t angle;
};

void drawParams(Guest& guest, DrawParams params) {
    float scale = params.scale;
    uint32_t color = params.color;
    int16_t angle = params.angle;
    uint32_t at = kFirstParticle + ParticleInterpolation::kDrawParams;
    guest.words[at + (4 * ParticleInterpolation::kScaleWords)] = word(scale);
    guest.words[at + (4 * ParticleInterpolation::kPrmColorWord)] = color;
    guest.words[at + (4 * ParticleInterpolation::kRotationWord)] =
        (uint32_t{static_cast<uint16_t>(angle)} << 16) | 0x0123;
}

void aParticlesScaleColourAndRotationArePaintedBetweenToo() {
    Guest guest;
    ParticleInterpolation particles = guest.interpolation();
    guest.emitter({{.x = 0.0f, .age = 3.0f}}, {.x = 0.0f, .age = 1.0f});
    drawParams(guest, {.scale = 1.0f, .color = 0x204060ffU, .angle = 0x7f00});
    particles.onEmitterDraw(kDraw);
    guest.emitter({{.x = 0.0f, .age = 4.0f}}, {.x = 0.0f, .age = 2.0f});
    drawParams(guest, {.scale = 3.0f, .color = 0x406080ffU, .angle = -0x7f00});
    particles.onMidPaintBegin(1);
    particles.onEmitterDraw(kDraw);
    uint32_t params = kFirstParticle + ParticleInterpolation::kDrawParams;
    check::isTrue(value(guest.words[params + (4 * ParticleInterpolation::kScaleWords)]) == 2.0f,
                  "a particle is painted at the scale between its two ticks'");
    check::isTrue(guest.words[params + (4 * ParticleInterpolation::kPrmColorWord)] == 0x305070ffU,
                  "in the colour between");
    check::isTrue(guest.words[params + (4 * ParticleInterpolation::kRotationWord)] ==
                      ((uint32_t{0x8000} << 16) | 0x0123),
                  "turned the short way round, its spin kept");
    particles.onMidPaintEnd();
    check::isTrue(value(guest.words[params + (4 * ParticleInterpolation::kScaleWords)]) == 3.0f &&
                      guest.words[params + (4 * ParticleInterpolation::kPrmColorWord)] ==
                          0x406080ffU,
                  "and the tick's own is back after");
}

} // namespace

void wiiuport::tests::runParticleInterpolationTests() {
    aParticleIsPaintedAtItsMidpointAndPutBack();
    aRenewedSlotIsPaintedAsItIs();
    onlyTheLastTicksPaintIsAnEnd();
    aParticlesScaleColourAndRotationArePaintedBetweenToo();
}
