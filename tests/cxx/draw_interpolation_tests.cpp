// The in-between picture's inputs: the camera's and each actor's, set to midpoints for the tick's
// draw phase and put back after it.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/DrawInterpolation.h"

#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <vector>

namespace {

using wiiuport::title::DrawInterpolation;

constexpr uint32_t kCamera = 0x3a000000;
constexpr uint32_t kActor = 0x3b000000;
constexpr uint32_t kModel = 0x3c000000;
constexpr uint32_t kSkeleton = 0x3d000000;
constexpr uint32_t kWorld = 0x3e000000;

using Matrix = std::array<float, 12>;

Matrix translation(float x, float y) {
    return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, 0};
}

// A quarter turn about z, then a move.
Matrix turned(float x, float y) {
    return {0, -1, 0, x, 1, 0, 0, y, 0, 0, 1, 0};
}

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

    DrawInterpolation
    interpolation(std::vector<DrawInterpolation::MidPaintListener*> midPaint = {},
                  std::vector<DrawInterpolation::DrawPhaseListener*> drawPhase = {}) {
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
                                      },
                                  .midPaint = std::move(midPaint),
                                  .drawPhase = std::move(drawPhase)}};
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

    // A model of one joint: its base and the joint's world matrix.
    void model(const Matrix& base, const Matrix& joint) {
        words[kModel + DrawInterpolation::kModelSkeleton] = kSkeleton;
        words[kSkeleton + DrawInterpolation::kSkeletonWorld] = kWorld;
        words[kSkeleton + DrawInterpolation::kSkeletonJoints] = 1U << 16;
        for (uint32_t i = 0; i < 12; ++i) {
            words[kModel + DrawInterpolation::kModelBase + (4 * i)] = word(base.at(i));
            words[kWorld + (4 * i)] = word(joint.at(i));
        }
    }

    float joint(uint32_t index) {
        return value(words[kWorld + (4 * index)]);
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

// The tick's draw phase, then the skipped call's.
void tick(DrawInterpolation& draw) {
    draw.onManagement();
    draw.onBeforeDraw();
    draw.onModelView(kModel);
    draw.onAfterDraw();
}

void skipped(DrawInterpolation& draw) {
    draw.onBeforeDraw();
    draw.onModelView(kModel);
    draw.onAfterDraw();
}

void aModelIsDrawnWithItsAnimationAndPlacementBlendedApart() {
    Guest guest;
    DrawInterpolation draw = guest.interpolation();
    // Joint 10 along the base's x, then the base turns and the joint reaches 20.
    guest.model(translation(0, 0), translation(10, 0));
    tick(draw);
    skipped(draw);
    guest.model(turned(0, 0), turned(0, 20));
    draw.onManagement();
    draw.onBeforeDraw();
    draw.onModelView(kModel);
    check::isTrue(guest.joint(3) == 7.5f && guest.joint(7) == 7.5f,
                  "the joint is the midpoint base times the midpoint model-space pose, not the "
                  "midpoint of its world matrices (5, 10)");
    draw.onAfterDraw();
    check::isTrue(guest.joint(3) == 0.0f && guest.joint(7) == 20.0f,
                  "and the tick's own matrices are back after the draw phase");
}

void aBaseFromTheDrawsInputsIsNotBlendedTwice() {
    Guest guest;
    DrawInterpolation draw = guest.interpolation();
    guest.model(translation(0, 0), translation(0, 0));
    tick(draw);
    skipped(draw);
    // The tick's draw places the model from an input already at its midpoint: 5 of 10.
    guest.model(translation(5, 0), translation(5, 0));
    draw.onManagement();
    draw.onBeforeDraw();
    draw.onModelView(kModel);
    draw.onAfterDraw();
    guest.model(translation(10, 0), translation(10, 0));
    skipped(draw);
    guest.model(translation(15, 0), translation(15, 0));
    draw.onManagement();
    draw.onBeforeDraw();
    draw.onModelView(kModel);
    check::isTrue(guest.joint(3) == 15.0f,
                  "a base that differs between the tick's draw and the skipped call's is the "
                  "draw's own midpoint, and is drawn as it is");
    draw.onAfterDraw();
}

void aModelSeenForTheFirstTimeIsDrawnAsItIs() {
    Guest guest;
    DrawInterpolation draw = guest.interpolation();
    guest.model(translation(0, 0), translation(10, 0));
    draw.onManagement();
    draw.onBeforeDraw();
    draw.onModelView(kModel);
    check::isTrue(guest.joint(3) == 10.0f, "a model with no tick before it is drawn as it is");
    draw.onAfterDraw();
}

// The window the paint drawn from the tick's draw phase falls in.
class PaintWindow final : public DrawInterpolation::MidPaintListener {
  public:
    std::vector<uint64_t> begun;
    uint64_t ended = 0;
    // The camera as the paint would read it when the window opens.
    float eyeAtBegin = 0.0f;
    std::map<uint32_t, uint32_t>* words = nullptr;

    void onMidPaintBegin(uint64_t tick) override {
        begun.push_back(tick);
        eyeAtBegin = value((*words)[kCamera + 0xdc]);
    }

    void onMidPaintEnd() override {
        ended++;
    }
};

void theInBetweenPaintIsTheOneAfterTheTicksDrawPhase() {
    Guest guest;
    PaintWindow window;
    window.words = &guest.words;
    DrawInterpolation draw = guest.interpolation({&window});
    guest.camera({.fovy = 60.0f, .eyeX = 100.0f, .bank = 0});
    draw.onManagement();
    draw.onBeforeDraw();
    draw.onCameraDraw(kCamera);
    draw.onAfterDraw();
    check::isTrue(window.begun == std::vector<uint64_t>{1},
                  "the tick's draw phase opens the in-between paint, with the tick");
    guest.camera({.fovy = 60.0f, .eyeX = 200.0f, .bank = 0});
    draw.onManagement();
    draw.onBeforeDraw();
    check::isTrue(window.ended == 1, "and the next draw phase closes it");
    draw.onCameraDraw(kCamera);
    draw.onAfterDraw();
    check::isTrue(window.eyeAtBegin == 200.0f,
                  "after the draw phase's own inputs are put back, so the paint's are its own");
    draw.onBeforeDraw();
    draw.onAfterDraw();
    check::isTrue(window.begun.size() == 2 && window.ended == 2,
                  "the skipped call's draw phase opens none");
    guest.gated = false;
    draw.onManagement();
    draw.onBeforeDraw();
    draw.onAfterDraw();
    check::isTrue(window.begun.size() == 2, "nor does a tick without the gate");
}

class DrawPhaseWindow final : public DrawInterpolation::DrawPhaseListener {
  public:
    std::vector<uint64_t> begun;
    uint64_t ended = 0;
    // The camera as the draw phase's listener finds it when it ends.
    float eyeAtEnd = 0.0f;
    std::map<uint32_t, uint32_t>* words = nullptr;

    void onDrawPhaseBegin(uint64_t tick) override {
        begun.push_back(tick);
    }

    void onDrawPhaseEnd() override {
        ended++;
        eyeAtEnd = value((*words)[kCamera + 0xdc]);
    }

    void onActorDraw(uint32_t actor) override {
        actors.push_back(actor);
    }

    std::vector<uint32_t> actors;
};

void theInBetweenDrawPhaseIsOpenedAndClosedForItsListeners() {
    Guest guest;
    DrawPhaseWindow window;
    window.words = &guest.words;
    PaintWindow paint;
    paint.words = &guest.words;
    DrawInterpolation draw = guest.interpolation({&paint}, {&window});
    guest.camera({.fovy = 60.0f, .eyeX = 100.0f, .bank = 0});
    draw.onManagement();
    draw.onBeforeDraw();
    check::isTrue(window.begun == std::vector<uint64_t>{1} && window.ended == 0,
                  "the tick's draw phase is opened at its start, with the tick");
    draw.onCameraDraw(kCamera);
    draw.onActorDraw(kActor);
    draw.onAfterDraw();
    check::isTrue(window.actors == std::vector<uint32_t>{kActor}, "with each actor it draws");
    check::isTrue(window.ended == 1 && paint.begun.size() == 1,
                  "and closed at its end, before the in-between paint");
    guest.camera({.fovy = 60.0f, .eyeX = 200.0f, .bank = 0});
    draw.onManagement();
    draw.onBeforeDraw();
    draw.onCameraDraw(kCamera);
    draw.onAfterDraw();
    check::isTrue(window.eyeAtEnd == 200.0f, "once the draw phase's own inputs are back");
    draw.onBeforeDraw();
    draw.onActorDraw(kActor);
    draw.onAfterDraw();
    check::isTrue(window.begun.size() == 2 && window.ended == 2 && window.actors.size() == 1,
                  "the skipped call's draw phase is not one");
    guest.gated = false;
    draw.onManagement();
    draw.onBeforeDraw();
    draw.onAfterDraw();
    check::isTrue(window.begun.size() == 2, "nor is a tick without the gate");
}

} // namespace

void wiiuport::tests::runDrawInterpolationTests() {
    theCameraIsDrawnFromTheMidpointOfTwoTicksAndPutBack();
    anExecutedActorIsDrawnBetweenItsOldAndCurrentPlacement();
    anActorThatDidNotRunIsDrawnAsItIs();
    nothingIsWrittenOutsideAGatedTick();
    aModelIsDrawnWithItsAnimationAndPlacementBlendedApart();
    aBaseFromTheDrawsInputsIsNotBlendedTwice();
    aModelSeenForTheFirstTimeIsDrawnAsItIs();
    theInBetweenPaintIsTheOneAfterTheTicksDrawPhase();
    theInBetweenDrawPhaseIsOpenedAndClosedForItsListeners();
}
