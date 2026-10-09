// The sea in the in-between draw phase: its grid between two ticks, its scroll stepped once a tick.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/SeaInterpolation.h"

#include <bit>
#include <cstdint>
#include <map>

namespace {

using wiiuport::title::SeaInterpolation;

constexpr uint32_t kPacket = 0x3c000000;
constexpr uint32_t kHeights = 0x3d000000;
// Initialised, not culled.
constexpr uint32_t kDrawn = 0x01000000;
constexpr uint32_t kCulled = 0x01010000;
// The last height in the grid.
constexpr uint32_t kLastHeight = kHeights + (4 * (SeaInterpolation::kHeights - 1));

uint32_t word(float value) {
    return std::bit_cast<uint32_t>(value);
}

float value(uint32_t word) {
    return std::bit_cast<float>(word);
}

struct Sea {
    float minX;
    float height;
    uint16_t counter;
};

class Guest {
  public:
    std::map<uint32_t, uint32_t> words;

    SeaInterpolation interpolation() {
        words[SeaInterpolation::kSeaPacket] = kPacket;
        words[kPacket + SeaInterpolation::kInitFlag] = kDrawn;
        words[kPacket + SeaInterpolation::kHeightTable] = kHeights;
        return SeaInterpolation{{.readWords =
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

    // What a tick's execute leaves: the grid's corner and its last height.
    void tick(Sea at) {
        words[kPacket + SeaInterpolation::kDrawMin] = word(at.minX);
        words[kPacket + SeaInterpolation::kDrawMin + 4] = word(0.0f);
        words[kLastHeight] = word(at.height);
        words[kPacket + SeaInterpolation::kAnimCounter] = uint32_t{at.counter} << 16;
    }

    // daSea_Draw stepping the scroll.
    void draw() {
        words[kPacket + SeaInterpolation::kAnimCounter] += 1U << 16;
    }

    float minX() {
        return value(words[kPacket + SeaInterpolation::kDrawMin]);
    }

    float height() {
        return value(words[kLastHeight]);
    }

    uint16_t counter() {
        return static_cast<uint16_t>(words[kPacket + SeaInterpolation::kAnimCounter] >> 16);
    }
};

void theSeaIsDrawnBetweenItsTwoTicksAndScrollsOnceATick() {
    Guest guest;
    SeaInterpolation sea = guest.interpolation();
    guest.tick({.minX = 0.0f, .height = 10.0f, .counter = 5});
    sea.onDrawPhaseBegin(1);
    check::isTrue(guest.minX() == 0.0f, "a sea with no tick before it is drawn as it is");
    guest.draw();
    sea.onDrawPhaseEnd();
    check::isTrue(guest.counter() == 5, "and its scroll is the tick's own after the draw phase");
    guest.draw();
    guest.tick({.minX = 40.0f, .height = 30.0f, .counter = guest.counter()});
    sea.onDrawPhaseBegin(2);
    check::isTrue(guest.minX() == 20.0f && guest.height() == 20.0f,
                  "the next is drawn with its grid where the player was between the ticks");
    guest.draw();
    sea.onDrawPhaseEnd();
    check::isTrue(guest.minX() == 40.0f && guest.height() == 30.0f && guest.counter() == 6,
                  "and the skipped call's draw phase gets the tick's own, one step on");
}

void aWarpedOrCulledSeaIsDrawnAsItIs() {
    Guest guest;
    SeaInterpolation sea = guest.interpolation();
    guest.tick({.minX = 0.0f, .height = 10.0f, .counter = 0});
    sea.onDrawPhaseBegin(1);
    sea.onDrawPhaseEnd();
    guest.tick({.minX = 5000.0f, .height = 30.0f, .counter = 0});
    sea.onDrawPhaseBegin(2);
    check::isTrue(guest.minX() == 5000.0f && guest.height() == 30.0f,
                  "a grid that jumped further than any travel is a warp");
    sea.onDrawPhaseEnd();
    guest.words[kPacket + SeaInterpolation::kInitFlag] = kCulled;
    sea.onDrawPhaseBegin(3);
    guest.draw();
    sea.onDrawPhaseEnd();
    check::isTrue(guest.counter() == 0, "a culled sea still scrolls once a tick");
    guest.words[kPacket + SeaInterpolation::kInitFlag] = kDrawn;
    guest.tick({.minX = 5010.0f, .height = 40.0f, .counter = 0});
    sea.onDrawPhaseBegin(4);
    check::isTrue(guest.minX() == 5010.0f, "and a sea back from culling starts again");
    sea.onDrawPhaseEnd();
}

} // namespace

void wiiuport::tests::runSeaInterpolationTests() {
    theSeaIsDrawnBetweenItsTwoTicksAndScrollsOnceATick();
    aWarpedOrCulledSeaIsDrawnAsItIs();
}
