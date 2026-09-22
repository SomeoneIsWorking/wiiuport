#include "check.h"
#include "suites.h"
#include "wiiuport/control/ControlChannel.h"
#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/frame/FramePresenter.h"
#include "wiiuport/frame/FrameReplayer.h"
#include "wiiuport/frame/ReplayScheduler.h"
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

bool refusePresent(const LatteFrameHooks::PresentArguments&) {
    return false;
}

wiiuport::interp::ContinuousInterpolator::Clock::time_point neverNow() {
    return {};
}

// Everything a channel needs, in one place. The constructor has widened
// three times as subsystems were added, and each time it widened in five
// tests at once; here it widens in one.
struct Fixture {
    RecordingObserver recorder;
    FrameReplayer replayer{&acceptEverySubmission};
    wiiuport::interp::TransformSearch search;
    wiiuport::input::InputDriver input;
    wiiuport::frame::FrameCapture capture{&refuseCapture};
    wiiuport::frame::FramePresenter presenter{&refusePresent};
    wiiuport::frame::ReplayScheduler scheduler{replayer, presenter, capture};
    wiiuport::interp::TransformSubstitution substitution;
    wiiuport::interp::FrameInterpolator interpolator{search, substitution, scheduler};
    wiiuport::frame::FrameShapeLog shapeLog;
    wiiuport::interp::ViewTracker viewTracker{search};
    wiiuport::interp::ObjectBlend objects{wiiuport::interp::ContinuousInterpolator::kBlendPoint};
    wiiuport::interp::ContinuousInterpolator continuous{
        viewTracker, substitution, objects, replayer, presenter, scheduler, &neverNow};
    wiiuport::frame::RecordingSnapshot snapshot;
    wiiuport::frame::PresentPacing pacing{&neverNow};
    ControlChannel channel{ControlChannel::Sources{
        .recorder = recorder,
        .replayer = replayer,
        .search = search,
        .input = input,
        .capture = capture,
        .presenter = presenter,
        .scheduler = scheduler,
        .interpolator = interpolator,
        .shapeLog = shapeLog,
        .viewTracker = viewTracker,
        .continuous = continuous,
        .snapshot = snapshot,
        .pacing = pacing,
    }};
};

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void anIdleRuntimeReportsZerosRatherThanNothing() {
    // The whole reason this route exists: a runtime that installed its hooks
    // and then saw nothing must be distinguishable from one that is working.
    // An empty body, or a route that only answers once there is something to
    // say, cannot tell those apart.
    Fixture fixture;
    std::string body = fixture.channel.countersJson();

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
    list.topLevel = true;

    Fixture fixture;
    fixture.recorder.OnDisplayList(list);
    fixture.recorder.OnFrameComplete();

    std::string body = fixture.channel.countersJson();
    check::isTrue(contains(body, "\"framesObserved\":1"), "the ended frame is counted");
    check::isTrue(contains(body, "\"displayListsSeen\":1"), "and so is its list");
    check::isTrue(contains(body, "\"lastFrameBytes\":16"), "with the bytes it held");
}

void aSearchThatFoundNothingStillSaysWhatItLookedAt() {
    // A bare "candidates: []" cannot be told from a search that never ran.
    // The denominators are the part that distinguishes them, so they are
    // asserted here rather than the empty list.
    Fixture fixture;
    auto body = fixture.channel.transformsJson(ControlChannel::kDefaultTransformLimit);
    check::isTrue(contains(body, "\"candidatesFound\":0"), "nothing was found");
    check::isTrue(contains(body, "\"framesObserved\":0"), "because no frame was watched");
    check::isTrue(contains(body, "\"spansExamined\":0"), "and nothing was examined");
    check::isTrue(contains(body, "\"shadersTracked\":0"), "across no shaders");
}

void aFoundTransformIsReportedWithItsValues() {
    Fixture fixture;
    for (auto x : {1.0f, 4.0f}) {
        wiiuport::frame::FrameRecording frame;
        wiiuport::frame::RecordedUniformAssembly assembly;
        assembly.shaderBaseHash = 0x1234;
        assembly.data = {1.0f, 0.0f, 0.0f, x, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        frame.addUniformAssembly(assembly);
        fixture.search.observe(frame);
    }
    auto body = fixture.channel.transformsJson(ControlChannel::kDefaultTransformLimit);
    check::isTrue(contains(body, "\"candidatesFound\":1"), "the moving transform is reported");
    check::isTrue(contains(body, "\"meanTranslationStep\":3"), "with how far it moved");
    check::isTrue(contains(body, "\"values\":[1,0,0,4,"), "and the values themselves");
}

// A setup screen as the channel sees one.
struct FakeSetup final : public wiiuport::control::SetupStatusSource {
    bool shown = true;
    std::string state = "rejected";
    size_t offered = 2;

    bool setupShown() const override {
        return shown;
    }

    std::string setupState() const override {
        return state;
    }

    size_t setupSelectionsOffered() const override {
        return offered;
    }
};

void aChannelWithNoSetupScreenSaysSoRatherThanReportingAClosedOne() {
    Fixture fixture;
    std::string body = fixture.channel.setupJson();
    check::isTrue(contains(body, "\"hostReports\":false"),
                  "a build with no host to show setup says nothing reported");
    check::isTrue(contains(body, "\"shown\":false"), "and does not claim a screen is up");
}

void aShownSetupScreenReportsWhatItIsWaitingFor() {
    Fixture fixture;
    FakeSetup setup;
    fixture.channel.setSetupStatus(&setup);
    std::string body = fixture.channel.setupJson();
    check::isTrue(contains(body, "\"hostReports\":true"), "a registered screen is reported");
    check::isTrue(contains(body, "\"shown\":true"), "as being on the display");
    check::isTrue(contains(body, "\"state\":\"rejected\""), "with what it is waiting for");
    // The denominator: a player who chose nothing and a player whose choice
    // was refused both leave the screen up.
    check::isTrue(contains(body, "\"selectionsOffered\":2"), "and how much it was handed");

    fixture.channel.setSetupStatus(nullptr);
    check::isTrue(contains(fixture.channel.setupJson(), "\"hostReports\":false"),
                  "and a screen that closed is no longer reported as one");
}

void anUnstartedChannelIsNotRunning() {
    Fixture fixture;
    check::isTrue(!fixture.channel.running(), "a channel nobody started is off");
    check::equal(fixture.channel.port(), uint16_t{0},
                 "and reports no port rather than a plausible one");
}

} // namespace

namespace wiiuport::tests {

void runControlTests() {
    anIdleRuntimeReportsZerosRatherThanNothing();
    theCountersFollowTheRecorder();
    aSearchThatFoundNothingStillSaysWhatItLookedAt();
    aFoundTransformIsReportedWithItsValues();
    aChannelWithNoSetupScreenSaysSoRatherThanReportingAClosedOne();
    aShownSetupScreenReportsWhatItIsWaitingFor();
    anUnstartedChannelIsNotRunning();
}

} // namespace wiiuport::tests
