// The in-between picture's inputs: the camera's and each actor's, set to midpoints for the tick's
// draw phase and put back after it.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/DrawInterpolation.h"

#include <bit>
#include <cstdint>
#include <map>

namespace {

using wiiuport::title::DrawInterpolation;

constexpr uint32_t kCamera = 0x3a000000;
constexpr uint32_t kActor = 0x3b000000;

struct CameraInputs {
    float fovy;
    float eyeX;
    int16_t bank;
};

struct ActorPlacement {
    float oldX;
    float currentX;
    int16_t shapeY;
    bool executed;
};

void noRegistration(uint32_t /*entry*/, uint32_t /*firstInstruction*/,
                    GuestCallProbes::Probe& /*probe*/, bool /*holdsEntry*/, uint32_t /*resume*/) {
}

uint32_t word(float value) {
    return std::bit_cast<uint32_t>(value);
}

float value(uint32_t word) {
    return std::bit_cast<float>(word);
}

class Guest {
  public:
    std::map<uint32_t, uint32_t> words;
    bool gated = true;

    DrawInterpolation interpolation() {
        return DrawInterpolation{{.registerProbe = &noRegistration,
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
                                      },
                                  .gated =
                                      [this] {
                                          return gated;
                                      }}};
    }

    // fovy, aspect, eye.x, then the bank in the high half.
    void camera(CameraInputs in) {
        words[kCamera + 0xd4] = word(in.fovy);
        words[kCamera + 0xd8] = word(1.5f);
        words[kCamera + 0xdc] = word(in.eyeX);
        for (uint32_t at = 0xe0; at < 0x100; at += 4) {
            words[kCamera + at] = word(1.0f);
        }
        words[kCamera + 0x100] = (uint32_t{static_cast<uint16_t>(in.bank)} << 16) | 0xabcd;
    }

    // old.pos.x, current.pos.x and shape_angle.y.
    void actor(ActorPlacement in) {
        words[kActor + 0x2e4] = in.executed ? 0 : DrawInterpolation::kNotExecuted;
        words[kActor + 0x300] = word(in.oldX);
        words[kActor + 0x304] = word(2.0f);
        words[kActor + 0x308] = word(3.0f);
        words[kActor + 0x314] = word(in.currentX);
        words[kActor + 0x318] = word(2.0f);
        words[kActor + 0x31c] = word(3.0f);
        words[kActor + 0x328] = static_cast<uint16_t>(in.shapeY);
        words[kActor + 0x32c] = 0;
    }
};

void theCameraIsDrawnFromTheMidpointOfTwoTicksAndPutBack() {
    Guest guest;
    DrawInterpolation draw = guest.interpolation();
    guest.camera({.fovy = 60.0f, .eyeX = 100.0f, .bank = 0x7f00});
    draw.onManagement();
    draw.onCameraDraw(kCamera);
    check::isTrue(value(guest.words[kCamera + 0xdc]) == 100.0f,
                  "a camera seen for the first time is drawn as it is");
    draw.onAfterDraw();
    guest.camera({.fovy = 70.0f, .eyeX = 200.0f, .bank = static_cast<int16_t>(-0x7f00)});
    draw.onManagement();
    draw.onCameraDraw(kCamera);
    check::isTrue(value(guest.words[kCamera + 0xdc]) == 150.0f &&
                      value(guest.words[kCamera + 0xd4]) == 65.0f,
                  "the next tick's draw sees the midpoint eye and fovy");
    check::isTrue(value(guest.words[kCamera + 0xd8]) == 1.5f, "and the aspect as it is");
    check::isTrue(guest.words[kCamera + 0x100] == ((uint32_t{0x8000} << 16) | 0xabcd),
                  "and the bank the short way round, through 0x8000, its padding kept");
    draw.onAfterDraw();
    check::isTrue(value(guest.words[kCamera + 0xdc]) == 200.0f &&
                      guest.words[kCamera + 0x100] == ((uint32_t{0x8100} << 16) | 0xabcd),
                  "and the tick's own inputs are back once the draw phase is done");
}

void anExecutedActorIsDrawnBetweenItsOldAndCurrentPlacement() {
    Guest guest;
    DrawInterpolation draw = guest.interpolation();
    guest.actor({.oldX = 0.0f, .currentX = 10.0f, .shapeY = 100, .executed = true});
    draw.onManagement();
    draw.onActorDraw(kActor);
    check::isTrue(guest.words[kActor + 0x328] == 100,
                  "an actor seen for the first time keeps its facing");
    draw.onAfterDraw();
    guest.actor({.oldX = 10.0f, .currentX = 30.0f, .shapeY = 300, .executed = true});
    draw.onManagement();
    draw.onActorDraw(kActor);
    check::isTrue(value(guest.words[kActor + 0x314]) == 20.0f &&
                      value(guest.words[kActor + 0x318]) == 2.0f,
                  "its position is the midpoint of old and current");
    check::isTrue(guest.words[kActor + 0x328] == 200, "and its facing the midpoint of two ticks'");
    check::isTrue(value(guest.words[kActor + 0x300]) == 10.0f, "and old is left as it is");
    draw.onAfterDraw();
    check::isTrue(value(guest.words[kActor + 0x314]) == 30.0f && guest.words[kActor + 0x328] == 300,
                  "and current is back after the draw phase");
}

void anActorThatDidNotRunIsDrawnAsItIs() {
    Guest guest;
    DrawInterpolation draw = guest.interpolation();
    guest.actor({.oldX = 0.0f, .currentX = 10.0f, .shapeY = 100, .executed = false});
    draw.onManagement();
    draw.onActorDraw(kActor);
    check::isTrue(value(guest.words[kActor + 0x314]) == 10.0f,
                  "an actor the title did not execute keeps its position: its old is stale");
    draw.onAfterDraw();
}

void nothingIsWrittenOutsideAGatedTick() {
    Guest guest;
    DrawInterpolation draw = guest.interpolation();
    guest.actor({.oldX = 0.0f, .currentX = 10.0f, .shapeY = 100, .executed = true});
    draw.onActorDraw(kActor);
    check::isTrue(value(guest.words[kActor + 0x314]) == 10.0f,
                  "the skipped call's draw phase draws the tick as it is");
    guest.gated = false;
    draw.onManagement();
    draw.onActorDraw(kActor);
    check::isTrue(value(guest.words[kActor + 0x314]) == 10.0f,
                  "and without the gate there is no in-between paint to draw for");
    draw.onAfterDraw();
    guest.gated = true;
    draw.setEnabled(false);
    draw.onManagement();
    draw.onActorDraw(kActor);
    check::isTrue(value(guest.words[kActor + 0x314]) == 10.0f, "and switched off it draws none");
    draw.onAfterDraw();
}

} // namespace

void wiiuport::tests::runDrawInterpolationTests() {
    theCameraIsDrawnFromTheMidpointOfTwoTicksAndPutBack();
    anExecutedActorIsDrawnBetweenItsOldAndCurrentPlacement();
    anActorThatDidNotRunIsDrawnAsItIs();
    nothingIsWrittenOutsideAGatedTick();
}
