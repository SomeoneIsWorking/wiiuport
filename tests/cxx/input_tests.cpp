#include "check.h"
#include "suites.h"
#include "wiiuport/input/InputDriver.h"

using wiiuport::input::InputDriver;

namespace {

VPADInputHooks::Injection polled(InputDriver& driver, bool& driven) {
    VPADInputHooks::Injection injection;
    driven = driver.Poll(0, injection);
    return injection;
}

void anUndrivenPlayerIsLeftToRealControllers() {
    // The product must behave exactly as upstream until a tool asks for
    // something. Reporting a zero hold mask instead of declining would
    // silently override whatever a physical controller was doing.
    InputDriver driver;
    auto driven = true;
    polled(driver, driven);
    check::isTrue(!driven, "an idle driver declines the player");
    check::equal(driver.pollsSeen(), uint64_t{1}, "having still seen the read");
    check::equal(driver.pollsAnswered(), uint64_t{0}, "and answered none");
}

void aPressIsHeldForTheRequestedReadsAndThenReleased() {
    InputDriver driver;
    driver.press(VPADInputHooks::kButtonA, 2);
    auto driven = false;
    check::equal(polled(driver, driven).holdMask, uint32_t{VPADInputHooks::kButtonA},
                 "the first read holds A");
    check::isTrue(driven, "and the player is driven");
    check::equal(polled(driver, driven).holdMask, uint32_t{VPADInputHooks::kButtonA},
                 "so does the second");
    // The release edge is the half a title actually acts on, so a press that
    // never ends is not a press.
    check::equal(polled(driver, driven).holdMask, uint32_t{0}, "the third has let go");
    check::isTrue(driven, "while the player stays driven");
}

void twoButtonsAreHeldTogether() {
    InputDriver driver;
    driver.press(VPADInputHooks::kButtonA, 4);
    driver.press(VPADInputHooks::kButtonPlus, 4);
    auto driven = false;
    check::equal(polled(driver, driven).holdMask,
                 uint32_t{VPADInputHooks::kButtonA | VPADInputHooks::kButtonPlus},
                 "both buttons are held at once");
}

void aStickIsHeldUntilChanged() {
    InputDriver driver;
    driver.setLeftStick(0.5f, -0.25f);
    auto driven = false;
    auto first = polled(driver, driven);
    check::near(first.leftStickX, 0.5f, 1e-6f, "the stick is driven");
    check::near(first.leftStickY, -0.25f, 1e-6f, "on both axes");
    auto second = polled(driver, driven);
    check::near(second.leftStickX, 0.5f, 1e-6f, "and stays put across reads");
}

void aCentredStickIsStillBeingDriven() {
    // Centring is an instruction, not an absence of one: a title being walked
    // forward has to be able to stop without handing the pad back.
    InputDriver driver;
    driver.setLeftStick(0.0f, 0.0f);
    auto driven = false;
    polled(driver, driven);
    check::isTrue(driven, "a centred stick still drives the player");
}

void releasingHandsThePlayerBack() {
    InputDriver driver;
    driver.press(VPADInputHooks::kButtonA, 100);
    driver.release();
    auto driven = true;
    polled(driver, driven);
    check::isTrue(!driven, "after release the player is undriven again");
    check::equal(driver.currentMask(), uint32_t{0}, "and nothing is held");
}

void onlyTheDrivenPlayerIsTouched() {
    InputDriver driver;
    driver.press(VPADInputHooks::kButtonA, 4);
    VPADInputHooks::Injection injection;
    check::isTrue(!driver.Poll(1, injection), "a second gamepad is not driven");
}

void anUnknownButtonNameIsRefused() {
    check::isTrue(InputDriver::buttonNamed("a") != nullptr, "a known name resolves");
    check::isTrue(InputDriver::buttonNamed("A") == nullptr, "and a typo does not");
    check::isTrue(InputDriver::buttonNamed("start") == nullptr, "nor a name from another console");
}

} // namespace

namespace wiiuport::tests {

void runInputTests() {
    anUndrivenPlayerIsLeftToRealControllers();
    aPressIsHeldForTheRequestedReadsAndThenReleased();
    twoButtonsAreHeldTogether();
    aStickIsHeldUntilChanged();
    aCentredStickIsStillBeingDriven();
    releasingHandsThePlayerBack();
    onlyTheDrivenPlayerIsTouched();
    anUnknownButtonNameIsRefused();
}

} // namespace wiiuport::tests
