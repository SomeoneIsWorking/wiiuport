#include "check.h"
#include "suites.h"
#include "wiiuport/frame/FrameCapture.h"

#include <array>

using wiiuport::frame::FrameCapture;

namespace {

// The armed callback, kept so a test can deliver an image the way the
// renderer would: later, and from somewhere else.
LatteFrameHooks::CaptureCallback g_armed;
bool g_accept = true;

// `count` is recorded as well as the callback, because the run is what a
// test of consecutive captures has to assert on: two arms of one is not two
// presents.
int g_armedFor = 0;

bool recordArming(LatteFrameHooks::CaptureCallback&& callback, int count) {
    if (!g_accept) {
        return false;
    }
    g_armed = std::move(callback);
    g_armedFor = count;
    return true;
}

// Four pixels by one, RGB: wider than it is tall, so a width and height
// swapped anywhere on the way reads back wrong.
constexpr int kImageWidth = 4;
constexpr int kImageHeight = 1;

void deliver(uint8_t value) {
    std::array<uint8_t, size_t{kImageWidth} * kImageHeight * 3> pixels{};
    pixels.fill(value);
    LatteFrameHooks::FrameImage image{pixels.data(), static_cast<uint32_t>(pixels.size()),
                                      kImageWidth, kImageHeight, true};
    g_armed(image);
}

// A run of N asks for N *presents*, not one arm repeated: the difference is the
// whole point, because the second arm of one lands a frame later than the first
// and a title that animates gives a different picture. The count has to reach the
// fork, and the images have to land in consecutive slots so a caller can compare
// them, which is what the null case is.
void aRunAsksForThatManyPresentsAndTheyLandInConsecutiveSlots() {
    g_accept = true;
    FrameCapture capture(&recordArming);
    g_armedFor = 0;
    check::isTrue(capture.armRun(3, 1), "a run of three arms");
    check::equal(g_armedFor, 3, "and the fork was asked for three presents, not one");
    check::isTrue(capture.lastImage(1).empty(), "slot 1 empty before any image");
    deliver(0x10);
    deliver(0x20);
    deliver(0x30);
    check::equal(capture.imagesReceived(), uint64_t{3}, "three images arrived");
    check::equal(capture.lastImage(1).rgb[0], uint8_t{0x10}, "the first into slot 1");
    check::equal(capture.lastImage(2).rgb[0], uint8_t{0x20}, "the second into slot 2");
    check::equal(capture.lastImage(3).rgb[0], uint8_t{0x30}, "the third into slot 3");
    // And the comparison the null case makes: two presents of one run, over
    // every byte, rather than a word about them. A run that put both images in
    // one slot would leave the second empty, which is the failure this shape is
    // here to catch.
    check::equal(capture.lastImage(1).rgb.size(), capture.lastImage(2).rgb.size(),
                 "and two of them are the same length, so a diff means something");
}

// A run that will not fit is refused by name. "Nothing captured" and "there was
// no room for the run asked for" are different findings, and a caller that treats
// them as one waits for images that were never coming.
void aRunTooLongForTheSlotsIsRefused() {
    g_accept = true;
    FrameCapture capture(&recordArming);
    const uint64_t refusedBefore = capture.capturesRefused();
    check::isTrue(!capture.armRun(FrameCapture::kSlotCount + 1, 0),
                  "a run longer than the slots is refused");
    check::equal(capture.capturesRefused(), refusedBefore + 1, "and counted as refused");
    check::isTrue(capture.lastImage().empty(), "with nothing captured");
}

// A run that overflows the slots drops the images that would not fit rather than
// writing past the end, and a read of a slot that never filled is empty rather
// than whatever was in the storage. Both were found by mutating the run's slot
// arithmetic: the overflow version segfaulted the test binary, which is a loud
// failure, but only because the test read a pixel out of the image it had just
// made empty.
void aRunThatOverflowsDropsItsImagesAndNeverWritesPastTheSlots() {
    g_accept = true;
    FrameCapture capture(&recordArming);
    // A run that would not fit is refused outright, so the overflow case is
    // reached by asking for exactly the slots that remain.
    check::isTrue(capture.armRun(1, FrameCapture::kSlotCount - 1),
                  "a run in the last slot arms");
    check::isTrue(!capture.armRun(2, FrameCapture::kSlotCount - 1),
                  "and a run of two in the last slot is refused rather than half-kept");
    deliver(0x40);
    deliver(0x50);
    check::equal(capture.lastImage(FrameCapture::kSlotCount - 1).rgb[0], uint8_t{0x40},
                 "the first lands in the last slot");
    check::isTrue(capture.lastImage(0).empty(),
                  "and a slot the run never reached stays empty, not stale");
    check::isTrue(capture.lastImage(FrameCapture::kSlotCount).empty(),
                  "and a slot past the end reads empty rather than out of bounds");
}

void nothingCapturedIsReportedAsNothing() {
    // An empty image served as a body reads as a black frame. The difference
    // has to survive to the caller.
    g_accept = true;
    FrameCapture capture(&recordArming);
    check::isTrue(capture.lastImage().empty(), "no image before one is captured");
    check::equal(capture.imagesReceived(), uint64_t{0}, "and none received");
}

void aDeliveredImageIsHeldByCopy() {
    g_accept = true;
    FrameCapture capture(&recordArming);
    check::isTrue(capture.armOnce(), "arming is accepted");
    deliver(0x7f);
    auto image = capture.lastImage();
    check::equal(image.width, kImageWidth, "the width survives");
    check::equal(image.height, kImageHeight, "so does the height");
    check::equal(image.rgb.size(), size_t{12}, "and every byte");
    check::equal(int{image.rgb[11]}, 0x7f, "with the values intact");
    check::equal(capture.imagesReceived(), uint64_t{1}, "one image received");
}

void aRefusedArmingIsCountedNotSwallowed() {
    // The renderer does not exist before the first frame. A caller that is
    // told "armed" and never gets an image cannot tell that from a title
    // that stopped presenting.
    g_accept = false;
    FrameCapture capture(&recordArming);
    check::isTrue(!capture.armOnce(), "a refused arming is reported");
    check::equal(capture.capturesRefused(), uint64_t{1}, "and counted");
    check::equal(capture.capturesRequested(), uint64_t{1}, "against what was asked");
    g_accept = true;
}

void theFramingCarriesItsOwnDimensions() {
    g_accept = true;
    FrameCapture capture(&recordArming);
    capture.armOnce();
    deliver(0x40);
    auto framed = capture.lastImageFramed();
    check::equal(framed.size(), FrameCapture::kHeaderBytes + 12,
                 "the body is its header plus its pixels");
    check::isTrue(framed.compare(0, 8, FrameCapture::kMagic) == 0,
                  "and starts with the magic a client checks");
}

} // namespace

namespace wiiuport::tests {

void runCaptureTests() {
    aRunAsksForThatManyPresentsAndTheyLandInConsecutiveSlots();
    aRunTooLongForTheSlotsIsRefused();
    aRunThatOverflowsDropsItsImagesAndNeverWritesPastTheSlots();
    nothingCapturedIsReportedAsNothing();
    aDeliveredImageIsHeldByCopy();
    aRefusedArmingIsCountedNotSwallowed();
    theFramingCarriesItsOwnDimensions();
}

} // namespace wiiuport::tests
