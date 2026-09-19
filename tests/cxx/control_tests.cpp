#include "check.h"
#include "suites.h"
#include "wiiuport/control/ControlChannel.h"
#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/frame/FrameReplayer.h"
#include "wiiuport/input/InputDriver.h"
#include "wiiuport/interp/TransformSearch.h"

#include <array>
#include <string>

using wiiuport::control::ControlChannel;
using wiiuport::frame::FrameReplayer;
using wiiuport::frame::RecordingObserver;

namespace {

bool acceptEverySubmission(const void*, uint32_t) {
    return true;
}

bool refuseCapture(LatteFrameHooks::CaptureCallback) {
    return false;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void anIdleRuntimeReportsZerosRatherThanNothing() {
    // The whole reason this route exists: a runtime that installed its hooks
    // and then saw nothing must be distinguishable from one that is working.
    // An empty body, or a route that only answers once there is something to
    // say, cannot tell those apart.
    RecordingObserver recorder;
    FrameReplayer replayer(&acceptEverySubmission);
    wiiuport::interp::TransformSearch search;
    wiiuport::input::InputDriver input;
    wiiuport::frame::FrameCapture capture(&refuseCapture);
    ControlChannel channel(recorder, replayer, search, input, capture);
    std::string body = channel.countersJson();

    check::isTrue(contains(body, "\"framesObserved\":0"), "frames observed is reported as zero");
    check::isTrue(contains(body, "\"displayListsSeen\":0"), "and so is the display list count");
    check::isTrue(contains(body, "\"lastFrameBytes\":0"), "and the last frame's size");
}

void theCountersFollowTheRecorder() {
    std::array<uint32_t, 4> guest{1, 2, 3, 4};
    LatteFrameHooks::DisplayList list{};
    list.physicalAddress = 0x40000000;
    list.data = guest.data();
    list.sizeInBytes = 16;

    RecordingObserver recorder;
    FrameReplayer replayer(&acceptEverySubmission);
    wiiuport::interp::TransformSearch search;
    wiiuport::input::InputDriver input;
    wiiuport::frame::FrameCapture capture(&refuseCapture);
    ControlChannel channel(recorder, replayer, search, input, capture);
    recorder.OnDisplayList(list);
    recorder.OnFrameEnd();

    std::string body = channel.countersJson();
    check::isTrue(contains(body, "\"framesObserved\":1"), "the ended frame is counted");
    check::isTrue(contains(body, "\"displayListsSeen\":1"), "and so is its list");
    check::isTrue(contains(body, "\"lastFrameBytes\":16"), "with the bytes it held");
}

void aSearchThatFoundNothingStillSaysWhatItLookedAt() {
    // A bare "candidates: []" cannot be told from a search that never ran.
    // The denominators are the part that distinguishes them, so they are
    // asserted here rather than the empty list.
    RecordingObserver recorder;
    FrameReplayer replayer(&acceptEverySubmission);
    wiiuport::interp::TransformSearch search;
    wiiuport::input::InputDriver input;
    wiiuport::frame::FrameCapture capture(&refuseCapture);
    ControlChannel channel(recorder, replayer, search, input, capture);
    auto body = channel.transformsJson(ControlChannel::kDefaultTransformLimit);
    check::isTrue(contains(body, "\"candidatesFound\":0"), "nothing was found");
    check::isTrue(contains(body, "\"framesObserved\":0"), "because no frame was watched");
    check::isTrue(contains(body, "\"spansExamined\":0"), "and nothing was examined");
    check::isTrue(contains(body, "\"shadersTracked\":0"), "across no shaders");
}

void aFoundTransformIsReportedWithItsValues() {
    RecordingObserver recorder;
    FrameReplayer replayer(&acceptEverySubmission);
    wiiuport::interp::TransformSearch search;
    wiiuport::input::InputDriver input;
    wiiuport::frame::FrameCapture capture(&refuseCapture);
    ControlChannel channel(recorder, replayer, search, input, capture);
    for (auto x : {1.0f, 4.0f}) {
        wiiuport::frame::FrameRecording frame;
        wiiuport::frame::RecordedUniformAssembly assembly;
        assembly.shaderBaseHash = 0x1234;
        assembly.data = {1.0f, 0.0f, 0.0f, x, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        frame.addUniformAssembly(assembly);
        search.observe(frame);
    }
    auto body = channel.transformsJson(ControlChannel::kDefaultTransformLimit);
    check::isTrue(contains(body, "\"candidatesFound\":1"), "the moving transform is reported");
    check::isTrue(contains(body, "\"meanTranslationStep\":3"), "with how far it moved");
    check::isTrue(contains(body, "\"values\":[1,0,0,4,"), "and the values themselves");
}

void anUnstartedChannelIsNotRunning() {
    RecordingObserver recorder;
    FrameReplayer replayer(&acceptEverySubmission);
    wiiuport::interp::TransformSearch search;
    wiiuport::input::InputDriver input;
    wiiuport::frame::FrameCapture capture(&refuseCapture);
    ControlChannel channel(recorder, replayer, search, input, capture);
    check::isTrue(!channel.running(), "a channel nobody started is off");
    check::equal(channel.port(), uint16_t{0}, "and reports no port rather than a plausible one");
}

} // namespace

namespace wiiuport::tests {

void runControlTests() {
    anIdleRuntimeReportsZerosRatherThanNothing();
    theCountersFollowTheRecorder();
    aSearchThatFoundNothingStillSaysWhatItLookedAt();
    aFoundTransformIsReportedWithItsValues();
    anUnstartedChannelIsNotRunning();
}

} // namespace wiiuport::tests
