// Material animation in the in-between draw phase: each controller an actor's draw entered, at the
// midpoint of its last two ticks' frames.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/MaterialInterpolation.h"

#include <bit>
#include <cstdint>
#include <map>

namespace {

using wiiuport::title::MaterialInterpolation;

constexpr uint32_t kActor = 0x3e000000;
constexpr uint32_t kAnimation = 0x3e000400;
constexpr uint32_t kJ3dAnimation = 0x3f000000;
// btk: the J3D animation at +0x68.
constexpr uint32_t kAnimationOffset = 0x68;

uint32_t word(float value) {
    return std::bit_cast<uint32_t>(value);
}

float value(uint32_t word) {
    return std::bit_cast<float>(word);
}

struct Controller {
    float rate;
    float frame;
    int16_t end;
    int16_t loop;
    uint8_t attribute;
};

class Guest {
  public:
    std::map<uint32_t, uint32_t> words;

    MaterialInterpolation interpolation() {
        words[kAnimation + kAnimationOffset] = kJ3dAnimation;
        return MaterialInterpolation{
            {.registerProbe = nullptr,
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

    // What the tick's execute leaves in the controller.
    void tick(Controller at) {
        words[kAnimation] = word(at.rate);
        words[kAnimation + 4] = word(at.frame);
        words[kAnimation + 8] = static_cast<uint16_t>(at.end);
        words[kAnimation + 12] =
            (uint32_t{static_cast<uint16_t>(at.loop)} << 16) | (uint32_t{at.attribute} << 8);
    }

    float frame() {
        return value(words[kAnimation + 4]);
    }

    // A gated draw phase in which the actor draws and its draw enters the animation.
    float draw(MaterialInterpolation& material, uint64_t tick) {
        material.onDrawPhaseBegin(tick);
        material.onActorDraw(kActor);
        float drawn = frame();
        material.onEntry(kAnimationOffset, kAnimation);
        material.onDrawPhaseEnd();
        return drawn;
    }
};

void anAnimationIsDrawnBetweenItsTwoTicksFrames() {
    Guest guest;
    MaterialInterpolation material = guest.interpolation();
    guest.tick({.rate = 1.0f, .frame = 10.0f, .end = 60, .loop = 0, .attribute = 2});
    check::isTrue(guest.draw(material, 1) == 10.0f, "an animation not yet entered draws as it is");
    guest.tick({.rate = 1.0f, .frame = 11.0f, .end = 60, .loop = 0, .attribute = 2});
    check::isTrue(guest.draw(material, 2) == 11.0f,
                  "and once entered, its first frame has nothing before it");
    guest.tick({.rate = 1.0f, .frame = 12.0f, .end = 60, .loop = 0, .attribute = 2});
    check::isTrue(guest.draw(material, 3) == 11.5f, "then it draws between its last two frames");
    check::isTrue(guest.frame() == 12.0f, "and the tick's own is back after the draw phase");
}

void aLoopingAnimationIsDrawnAcrossItsWrap() {
    Guest guest;
    MaterialInterpolation material = guest.interpolation();
    guest.tick({.rate = 1.0f, .frame = 58.0f, .end = 60, .loop = 0, .attribute = 2});
    guest.draw(material, 1);
    guest.tick({.rate = 1.0f, .frame = 59.0f, .end = 60, .loop = 0, .attribute = 2});
    guest.draw(material, 2);
    guest.tick({.rate = 1.0f, .frame = 0.0f, .end = 60, .loop = 0, .attribute = 2});
    check::isTrue(guest.draw(material, 3) == 59.5f, "a loop is drawn on its way round");
}

void aJumpOrAReplacedAnimationIsDrawnAsItIs() {
    Guest guest;
    MaterialInterpolation material = guest.interpolation();
    guest.tick({.rate = 1.0f, .frame = 10.0f, .end = 60, .loop = 0, .attribute = 0});
    guest.draw(material, 1);
    guest.tick({.rate = 1.0f, .frame = 11.0f, .end = 60, .loop = 0, .attribute = 0});
    guest.draw(material, 2);
    guest.tick({.rate = 1.0f, .frame = 40.0f, .end = 60, .loop = 0, .attribute = 0});
    check::isTrue(guest.draw(material, 3) == 40.0f, "a frame set by the actor is a jump");
    guest.tick({.rate = 1.0f, .frame = 41.0f, .end = 60, .loop = 0, .attribute = 0});
    guest.words[kAnimation + kAnimationOffset] = kJ3dAnimation + 0x100;
    check::isTrue(guest.draw(material, 4) == 41.0f,
                  "and an animation holding another J3D animation is not the one entered");
}

// An actor that plays its animation in its draw: both draw phases, gated then skipped.
float drawWithPlay(Guest& guest, MaterialInterpolation& material, uint64_t tick) {
    material.onDrawPhaseBegin(tick);
    material.onActorDraw(kActor);
    material.onPlay(kAnimation);
    guest.words[kAnimation + 4] = word(guest.frame() + 1.0f);
    float drawn = guest.frame();
    material.onEntry(kAnimationOffset, kAnimation);
    material.onDrawPhaseEnd();
    guest.words[kAnimation + 4] = word(guest.frame() + 1.0f);
    return drawn;
}

void anAnimationPlayedInTheDrawStepsOnceATickAndIsDrawnBetween() {
    Guest guest;
    MaterialInterpolation material = guest.interpolation();
    guest.tick({.rate = 1.0f, .frame = 10.0f, .end = 60, .loop = 0, .attribute = 2});
    drawWithPlay(guest, material, 1);
    float before = guest.frame();
    float drawn = drawWithPlay(guest, material, 2);
    check::isTrue(drawn == before + 0.5f, "its in-between draw plays to the midpoint");
    check::isTrue(guest.frame() == before + 1.0f, "and the tick steps it once");
    before = guest.frame();
    drawn = drawWithPlay(guest, material, 3);
    check::isTrue(drawn == before + 0.5f && guest.frame() == before + 1.0f,
                  "tick after tick, with no blend of its own on top");
}

} // namespace

void wiiuport::tests::runMaterialInterpolationTests() {
    anAnimationIsDrawnBetweenItsTwoTicksFrames();
    aLoopingAnimationIsDrawnAcrossItsWrap();
    aJumpOrAReplacedAnimationIsDrawnAsItIs();
    anAnimationPlayedInTheDrawStepsOnceATickAndIsDrawnBetween();
}
