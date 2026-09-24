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

bool recordArming(LatteFrameHooks::CaptureCallback&& callback) {
    if (!g_accept) {
        return false;
    }
    g_armed = std::move(callback);
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
    nothingCapturedIsReportedAsNothing();
    aDeliveredImageIsHeldByCopy();
    aRefusedArmingIsCountedNotSwallowed();
    theFramingCarriesItsOwnDimensions();
}

} // namespace wiiuport::tests
