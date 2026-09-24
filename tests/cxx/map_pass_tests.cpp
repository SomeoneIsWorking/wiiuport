#include "check.h"
#include "suites.h"
#include "wiiuport/interp/MapPassValues.h"

#include <cstdint>
#include <vector>

using wiiuport::interp::MapPassValues;

namespace {

constexpr uint64_t kBoat = 0x48729e00;
constexpr uint64_t kPier = 0x4872a000;

// Each draw into the map: its object's placement, then the light.
std::vector<float> caster(float placement, float light) {
    return {placement, 1.0f, light};
}

void aValueTwoObjectsMovedAlikeIsHeldAndOneObjectsOwnIsBlended() {
    MapPassValues pass;
    pass.add(kBoat, caster(0.0f, 10.0f), caster(2.0f, 12.0f));
    pass.add(kPier, caster(5.0f, 10.0f), caster(5.0f, 12.0f));
    std::vector<float> blended = {1.0f, 1.0f, 11.0f};
    size_t held = pass.holdThePass(caster(0.0f, 10.0f), caster(2.0f, 12.0f), blended);
    check::equal(held, size_t{1}, "the light alone is put back");
    check::equal(blended[2], 12.0f, "at N, as the look-up holds it");
    check::equal(blended[0], 1.0f, "while the boat's own placement stays blended");
}

void anObjectDrawnIntoEveryCascadeKeepsItsOwnValues() {
    // One object drawn into three cascades moves its placement in all three:
    // one object, not a pass.
    MapPassValues pass;
    for (int cascade = 0; cascade < 3; ++cascade) {
        pass.add(kBoat, caster(0.0f, 10.0f), caster(2.0f, 12.0f));
    }
    std::vector<float> blended = {1.0f, 1.0f, 11.0f};
    check::equal(pass.holdThePass(caster(0.0f, 10.0f), caster(2.0f, 12.0f), blended), size_t{0},
                 "nothing another object moved is put back");
    check::equal(blended[0], 1.0f, "so the placement stays blended");
    check::equal(blended[2], 11.0f, "and so does a light no other object holds");
}

void aValueMovedToAnotherEndIsNotThePasses() {
    // The same N-2 value moved elsewhere by another object is that object's
    // own move, not one the pass handed both.
    MapPassValues pass;
    pass.add(kBoat, caster(0.0f, 10.0f), caster(2.0f, 12.0f));
    pass.add(kPier, caster(0.0f, 10.0f), caster(3.0f, 13.0f));
    std::vector<float> blended = {1.0f, 1.0f, 11.0f};
    check::equal(pass.holdThePass(caster(0.0f, 10.0f), caster(2.0f, 12.0f), blended), size_t{0},
                 "ends that differ are different moves");
}

void aDrawThatMovedOnlyWithThePassIsToldFromOneThatMovedOfItself() {
    MapPassValues pass;
    pass.add(kBoat, caster(0.0f, 10.0f), caster(2.0f, 12.0f));
    pass.add(kPier, caster(5.0f, 10.0f), caster(5.0f, 12.0f));
    check::isTrue(pass.movesOnlyThePass(caster(5.0f, 10.0f), caster(5.0f, 12.0f)),
                  "the pier moved only the light");
    check::isTrue(!pass.movesOnlyThePass(caster(0.0f, 10.0f), caster(2.0f, 12.0f)),
                  "the boat moved of itself too");
    check::isTrue(!pass.movesOnlyThePass(caster(5.0f, 12.0f), caster(5.0f, 12.0f)),
                  "and a draw that moved nothing is not said to move with the pass");
}

} // namespace

namespace wiiuport::tests {

void runMapPassTests() {
    aValueTwoObjectsMovedAlikeIsHeldAndOneObjectsOwnIsBlended();
    anObjectDrawnIntoEveryCascadeKeepsItsOwnValues();
    aValueMovedToAnotherEndIsNotThePasses();
    aDrawThatMovedOnlyWithThePassIsToldFromOneThatMovedOfItself();
}

} // namespace wiiuport::tests
