#include "check.h"
#include "suites.h"
#include "wiiuport/control/ControlChannel.h"
#include "wiiuport/control/GuestMemoryRead.h"
#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/frame/FramePresenter.h"
#include "wiiuport/frame/FrameReplayer.h"
#include "wiiuport/frame/GuestStateGuard.h"
#include "wiiuport/frame/ReplayScheduler.h"
#include "wiiuport/input/InputDriver.h"
#include "wiiuport/interp/TransformSearch.h"

#include <array>
#include <string>
#include <vector>

using wiiuport::control::ControlChannel;
using wiiuport::frame::FrameReplayer;
using wiiuport::frame::RecordingObserver;

namespace {

bool acceptEverySubmission(const void*, uint32_t) {
    return true;
}

bool refuseCapture(LatteFrameHooks::CaptureCallback&&) {
    return false;
}

bool refusePresent(const LatteFrameHooks::PresentArguments&) {
    return false;
}

void noRegistration(uint32_t /*entry*/, uint32_t /*firstInstruction*/,
                    GuestCallProbes::Probe& /*probe*/) {
}

const void* noGuestBytes(uint32_t /*address*/, uint32_t /*size*/) {
    return nullptr;
}

void noGuard() {
}

LatteFrameHooks::GuestStateRestore noRestore() {
    return {};
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
    wiiuport::guest::BufferWriters writers;
    wiiuport::guest::CallerCensus callers{&noRegistration};
    wiiuport::interp::VertexBlend vertices{objects, writers,
                                           wiiuport::interp::ContinuousInterpolator::kBlendPoint};
    wiiuport::frame::GuestStateGuard guard{&noGuard, &noRestore};
    wiiuport::interp::RestoreCheck restoreCheck{presenter, capture};
    wiiuport::interp::NeighbourCheck neighbourCheck{capture};
    wiiuport::interp::ContinuousInterpolator continuous{viewTracker, substitution, objects,
                                                        replayer,    presenter,    guard,
                                                        scheduler,   restoreCheck, &neverNow};
    wiiuport::frame::RecordingSnapshot snapshot;
    wiiuport::frame::PresentPacing pacing{&neverNow};
    wiiuport::frame::PresentPacing scanOut{&neverNow};
    wiiuport::frame::FrameGate gate;
    wiiuport::interp::ShadowCheck shadowCheck{
        [] {
            return std::vector<wiiuport::frame::GuestMemorySnapshot::Region>{};
        },
        [](const wiiuport::frame::FrameRecording&) {
            return false;
        },
        [] {
            return wiiuport::interp::ShadowCheck::Clock::time_point{};
        },
        [](wiiuport::interp::ShadowCheck::Clock::duration) {
        }};
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
        .restoreCheck = restoreCheck,
        .neighbourCheck = neighbourCheck,
        .objects = objects,
        .vertices = vertices,
        .writers = writers,
        .callers = callers,
        .guestBytes = &noGuestBytes,
        .snapshot = snapshot,
        .pacing = pacing,
        .scanOut = scanOut,
        .vertexChanges = recorder.vertexChanges(),
        .gate = gate,
        .shadowCheck = shadowCheck,
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

// A host as the channel sees one: it counts the stops it was asked for.
struct FakeHost final : public wiiuport::control::HostStopTarget {
    int stopsRequested = 0;

    void requestStop() override {
        ++stopsRequested;
    }
};

void aQuitWithNoHostRunningIsRefusedAndAQuitWithOneReachesIt() {
    Fixture fixture;
    bool accepted = true;
    std::string body = fixture.channel.requestHostStop(accepted);
    check::isTrue(!accepted, "with no title running there is no host to stop");
    check::isTrue(contains(body, "\"stopping\":false"), "and the refusal says so");

    FakeHost host;
    fixture.channel.setHostStop(&host);
    body = fixture.channel.requestHostStop(accepted);
    check::isTrue(accepted, "a registered host takes the request");
    check::equal(host.stopsRequested, 1, "and is asked to stop exactly once");
    check::isTrue(contains(body, "\"stopping\":true"), "which the reply reports");

    fixture.channel.setHostStop(nullptr);
    fixture.channel.requestHostStop(accepted);
    check::isTrue(!accepted, "a host that unregistered is not asked again");
    check::equal(host.stopsRequested, 1, "and was not");
}

void anUnstartedChannelIsNotRunning() {
    Fixture fixture;
    check::isTrue(!fixture.channel.running(), "a channel nobody started is off");
    check::equal(fixture.channel.port(), uint16_t{0},
                 "and reports no port rather than a plausible one");
}

void aCensusTakesEntriesWithTheirFirstInstructionAndRefusesAnythingElse() {
    using wiiuport::guest::CallerCensus;
    std::string refusal;
    auto two = CallerCensus::parse("020350c4:7c0802a6,025df948:7c0802a6", refusal);
    check::isTrue(two.has_value() && two->size() == 2, "two targets are two");
    if (!two.has_value() || two->size() != 2) {
        return;
    }
    check::equal((*two)[1].entry, uint32_t{0x025df948}, "each at its entry");
    check::equal((*two)[1].firstInstruction, uint32_t{0x7c0802a6}, "expecting its instruction");
    auto none = CallerCensus::parse("", refusal);
    check::isTrue(none.has_value() && none->empty(), "nothing configured is no census");
    check::isTrue(!CallerCensus::parse("020350c4", refusal).has_value(),
                  "an entry without its instruction is refused");
    check::isTrue(!refusal.empty(), "by reason");
    check::isTrue(!CallerCensus::parse("020350c4:zz", refusal).has_value(),
                  "as is one that is not hex");
    check::isTrue(!CallerCensus::parse("1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9", refusal).has_value(),
                  "and a ninth target");
}

void anIdleCensusReportsNoEntriesRatherThanNothing() {
    Fixture fixture;
    check::equal(fixture.callers.json(), std::string("{\"entries\":[]}\n"),
                 "a census with nothing configured says so");
}

void aMemoryReadNamesItsRangeAndIsBounded() {
    using wiiuport::control::GuestMemoryRead;
    std::string refusal;
    auto read = GuestMemoryRead::parse("address=1004f74&size=64", refusal);
    check::isTrue(read.has_value(), "an address in hex and a size in decimal is a read");
    if (!read.has_value()) {
        return;
    }
    check::equal(read->address, uint32_t{0x01004f74}, "of that address");
    check::equal(read->size, uint32_t{64}, "and that many bytes");
    check::isTrue(!GuestMemoryRead::parse("size=64", refusal).has_value(), "no address, no read");
    check::isTrue(!GuestMemoryRead::parse("address=10&size=0", refusal).has_value(),
                  "nor zero bytes");
    check::isTrue(!GuestMemoryRead::parse("address=10&size=16777217", refusal).has_value(),
                  "nor more than the bound");
    check::isTrue(!GuestMemoryRead::parse("address=ffffffff&size=2", refusal).has_value(),
                  "nor past the end of the address space");
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
    aQuitWithNoHostRunningIsRefusedAndAQuitWithOneReachesIt();
    anUnstartedChannelIsNotRunning();
    aCensusTakesEntriesWithTheirFirstInstructionAndRefusesAnythingElse();
    anIdleCensusReportsNoEntriesRatherThanNothing();
    aMemoryReadNamesItsRangeAndIsBounded();
}

} // namespace wiiuport::tests
