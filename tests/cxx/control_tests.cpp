#include "check.h"
#include "suites.h"
#include "wiiuport/control/ControlChannel.h"
#include "wiiuport/frame/FrameReplayer.h"

#include <array>
#include <string>

using wiiuport::control::ControlChannel;
using wiiuport::frame::FrameReplayer;
using wiiuport::frame::RecordingObserver;

namespace {

bool acceptEverySubmission(const void*, uint32_t) {
    return true;
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
    ControlChannel channel(recorder, replayer);
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
    ControlChannel channel(recorder, replayer);
    recorder.OnDisplayList(list);
    recorder.OnFrameEnd();

    std::string body = channel.countersJson();
    check::isTrue(contains(body, "\"framesObserved\":1"), "the ended frame is counted");
    check::isTrue(contains(body, "\"displayListsSeen\":1"), "and so is its list");
    check::isTrue(contains(body, "\"lastFrameBytes\":16"), "with the bytes it held");
}

void anUnstartedChannelIsNotRunning() {
    RecordingObserver recorder;
    FrameReplayer replayer(&acceptEverySubmission);
    ControlChannel channel(recorder, replayer);
    check::isTrue(!channel.running(), "a channel nobody started is off");
    check::equal(channel.port(), uint16_t{0}, "and reports no port rather than a plausible one");
}

} // namespace

namespace wiiuport::tests {

void runControlTests() {
    anIdleRuntimeReportsZerosRatherThanNothing();
    theCountersFollowTheRecorder();
    anUnstartedChannelIsNotRunning();
}

} // namespace wiiuport::tests
