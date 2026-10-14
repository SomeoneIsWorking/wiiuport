// The grappling rope in the in-between draw phase: placed between two ticks, its sway eased once a
// tick.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/RopeInterpolation.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>

namespace {

using wiiuport::title::RopeInterpolation;

constexpr uint32_t kActor = 0x30000000;
constexpr uint32_t kHook = kActor + RopeInterpolation::kHandHook + 12;
constexpr uint32_t kSway = kActor + RopeInterpolation::kSway;

uint32_t word(float value) {
    return std::bit_cast<uint32_t>(value);
}

float value(uint32_t word) {
    return std::bit_cast<float>(word);
}

uint32_t segmentX(uint32_t index) {
    return kActor + RopeInterpolation::kSegments + (index * RopeInterpolation::kSegmentStride) +
           RopeInterpolation::kSegmentPosition;
}

class Guest {
  public:
    std::map<uint32_t, uint32_t> words;

    RopeInterpolation interpolation() {
        words[kActor + RopeInterpolation::kProcessName] = RopeInterpolation::kHimo2 << 16;
        return RopeInterpolation{{.readWords =
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

    // What a tick's execute leaves: the hook, the first live segment and two segments' x.
    void tick(float hook, uint32_t first, float paidOut, float last) {
        words[kHook] = word(hook);
        words[kActor + RopeInterpolation::kFirstSegment] = first;
        words[segmentX(first)] = word(paidOut);
        words[segmentX(RopeInterpolation::kSegmentCount - 1)] = word(last);
    }

    // daHimo2_c::draw easing the sway.
    void draw() {
        words[kSway] = word(value(words[kSway]) + 1.0f);
    }

    float at(uint32_t address) {
        return value(words[address]);
    }
};

void theRopeIsDrawnBetweenItsTwoTicksAndSwaysOnceATick() {
    Guest guest;
    RopeInterpolation rope = guest.interpolation();
    guest.tick(0.0f, 90, 0.0f, 10.0f);
    rope.onDrawPhaseBegin(1);
    rope.onActorDraw(kActor);
    check::isTrue(guest.at(kHook) == 0.0f, "a rope with no tick before it is drawn as it is");
    guest.draw();
    rope.onDrawPhaseEnd();
    check::isTrue(guest.at(kSway) == 0.0f, "and its sway is the tick's own after the draw phase");
    guest.draw();
    guest.tick(40.0f, 90, 20.0f, 30.0f);
    rope.onDrawPhaseBegin(2);
    rope.onActorDraw(kActor);
    check::isTrue(guest.at(kHook) == 20.0f && guest.at(segmentX(90)) == 10.0f &&
                      guest.at(segmentX(RopeInterpolation::kSegmentCount - 1)) == 20.0f,
                  "the next is drawn with hook and segments between the ticks");
    guest.draw();
    rope.onDrawPhaseEnd();
    check::isTrue(guest.at(kHook) == 40.0f && guest.at(segmentX(90)) == 20.0f &&
                      guest.at(kSway) == 1.0f,
                  "and the skipped call's draw phase gets the tick's own, swayed once");
}

void onlyTheRopeAndSegmentsBothTicksPaidOutAreBlended() {
    Guest guest;
    RopeInterpolation rope = guest.interpolation();
    guest.tick(0.0f, 90, 0.0f, 10.0f);
    rope.onDrawPhaseBegin(1);
    rope.onActorDraw(kActor);
    rope.onDrawPhaseEnd();
    guest.tick(40.0f, 80, 50.0f, 30.0f);
    rope.onDrawPhaseBegin(2);
    rope.onActorDraw(kActor);
    check::isTrue(guest.at(segmentX(80)) == 50.0f && guest.at(kHook) == 20.0f,
                  "a segment paid out this tick is drawn where it is");
    rope.onDrawPhaseEnd();
    guest.words[kHook] = word(std::numeric_limits<float>::quiet_NaN());
    rope.onDrawPhaseBegin(3);
    rope.onActorDraw(kActor);
    check::isTrue(std::isnan(guest.at(kHook)) && guest.at(segmentX(80)) == 50.0f,
                  "a rope with a value that cannot be blended is drawn as it is");
    rope.onDrawPhaseEnd();
    guest.words[kActor + RopeInterpolation::kProcessName] = 445U << 16;
    guest.tick(80.0f, 80, 60.0f, 30.0f);
    rope.onDrawPhaseBegin(4);
    rope.onActorDraw(kActor);
    check::isTrue(guest.at(kHook) == 80.0f, "another actor is not the rope");
    rope.onDrawPhaseEnd();
}

} // namespace

void wiiuport::tests::runRopeInterpolationTests() {
    theRopeIsDrawnBetweenItsTwoTicksAndSwaysOnceATick();
    onlyTheRopeAndSegmentsBothTicksPaidOutAreBlended();
}
