// The product's sixty: the second paint and the logic gate together, or neither.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/SixtyHertz.h"

#include <string>

namespace {

using wiiuport::title::SixtyHertz;

struct Halves {
    std::string paintRefusal;
    std::string gateRefusal;
    bool paint = false;
    bool gate = false;

    SixtyHertz sixty() {
        return SixtyHertz{{.enablePaint =
                               [this] {
                                   paint = paintRefusal.empty();
                                   return paintRefusal;
                               },
                           .disablePaint =
                               [this] {
                                   paint = false;
                                   return std::string{};
                               },
                           .enableGate =
                               [this] {
                                   gate = gateRefusal.empty();
                                   return gateRefusal;
                               }}};
    }
};

void bothHalvesGoIn() {
    Halves halves;
    check::isTrue(halves.sixty().switchOn().empty() && halves.paint && halves.gate,
                  "the second paint and the gate both in");
}

void aRefusedPaintLeavesTheGateOut() {
    Halves halves{.paintRefusal = "no display"};
    std::string refusal = halves.sixty().switchOn();
    check::isTrue(!refusal.empty() && !halves.paint && !halves.gate,
                  "no gate without the second paint: the logic would run at fifteen");
}

void aRefusedGateTakesThePaintBackOut() {
    Halves halves{.gateRefusal = "no memory"};
    std::string refusal = halves.sixty().switchOn();
    check::isTrue(!refusal.empty() && !halves.paint && !halves.gate,
                  "no second paint without the gate: the logic would run at sixty");
}

} // namespace

void wiiuport::tests::runSixtyHertzTests() {
    bothHalvesGoIn();
    aRefusedPaintLeavesTheGateOut();
    aRefusedGateTakesThePaintBackOut();
}
