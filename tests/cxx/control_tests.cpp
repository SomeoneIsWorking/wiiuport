#include "check.h"
#include "lucent/http.h"
#include "suites.h"
#include "wiiuport/control/ControlChannel.h"
#include "wiiuport/control/GuestMemoryRead.h"
#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/input/InputDriver.h"

#include <array>
#include <string>
#include <vector>

using wiiuport::control::ControlChannel;
using wiiuport::frame::RecordingObserver;

namespace {

bool refuseCapture(LatteFrameHooks::CaptureCallback&&, int) {
    return false;
}

void noRegistration(uint32_t /*entry*/, uint32_t /*firstInstruction*/,
                    GuestCallProbes::Probe& /*probe*/, bool /*holdsEntry*/ = true,
                    uint32_t /*resume*/ = 0) {
}

uint32_t noCodeSpace(uint32_t /*sizeInBytes*/) {
    return 0;
}

bool noWriteWord(uint32_t /*address*/, uint32_t /*value*/) {
    return false;
}

bool noReadWord(uint32_t /*address*/, uint32_t& /*value*/) {
    return false;
}

// A frame counter that never moves, so a scan asked through this fixture reports the refusal it
// gives when the two readings would be the same instant -- the refusal a test needs, not a zero.
uint64_t noFrame() {
    return 0;
}

bool noReadWords(uint32_t /*address*/, uint32_t* /*values*/, uint32_t /*count*/) {
    return false;
}

uint32_t noPacingChange(uint32_t vblanks) {
    return vblanks;
}

uint32_t noPacing() {
    return 2;
}

const void* noGuestBytes(uint32_t /*address*/, uint32_t /*size*/) {
    return nullptr;
}

void noGuard() {
}

LatteFrameHooks::GuestStateRestore noRestore() {
    return {};
}

// The clock the channel's pacing and frame gate are constructed with. It was the retired
// interpolator's clock first; it is kept here as a seam rather than replaced, because the channel
// still takes one and a test wiring a different one would be testing a different channel.
std::chrono::steady_clock::time_point neverNow() {
    return {};
}

// Everything a channel needs, in one place. The constructor has widened
// three times as subsystems were added, and each time it widened in five
// tests at once; here it widens in one.
struct Fixture {
    RecordingObserver recorder;
    wiiuport::input::InputDriver input;
    wiiuport::frame::FrameCapture capture{&refuseCapture};
    wiiuport::frame::FrameShapeLog shapeLog;
    wiiuport::guest::BufferWriters writers;
    wiiuport::guest::CallerCensus callers{&noRegistration};
    wiiuport::title::UniformBlockCensus blocks{&noRegistration, &noReadWord, &noReadWords};
    // The data-area scan, with readers that refuse: a channel built with readers that say no is how
    // every refusal in this file is exercised, and a scan wired with a reader that answers would
    // never reach the refusal it exists to report.
    wiiuport::title::GlobalPoseCensus globalPose{&noReadWords, &noFrame};
    wiiuport::title::LogicGate logic{&noRegistration, &noCodeSpace, &noCodeSpace, &noWriteWord,
                                     &noReadWord};
    wiiuport::title::WindWakerPaint paint{&noRegistration, &noCodeSpace,    &noWriteWord,
                                          &noReadWord,     &noPacingChange, &noPacing};
    wiiuport::frame::RecordingSnapshot snapshot;
    wiiuport::frame::PresentPacing pacing{&neverNow};
    wiiuport::frame::PresentPacing scanOut{&neverNow};
    wiiuport::frame::FrameGate gate;
    ControlChannel channel{ControlChannel::Sources{
        .recorder = recorder,
        .input = input,
        .capture = capture,
        .shapeLog = shapeLog,
        .writers = writers,
        .callers = callers,
        .paint = paint,
        .blocks = blocks,
        .globalPose = globalPose,
        .logic = logic,
        .guestBytes = &noGuestBytes,
        .snapshot = snapshot,
        .pacing = pacing,
        .scanOut = scanOut,
        .vertexChanges = recorder.vertexChanges(),
        .gate = gate,
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

// Every route the channel advertises, asked for by its own method, must reach a
// handler. The one it cannot reach is the kind of mistake that costs a nine
// minute run to find: the refusal names the route, the client sends it, and the
// answer is that the route does not exist.
//
// The test asks each entry of the channel's own list and counts what matched, so
// a list that shrank cannot pass by having nothing left to check. A route may
// still refuse the *request* -- a read with no range, a gate with no pause -- and
// that is a different answer, told apart by the refusal's own text rather than
// by its status, because a legitimate 404 (a range that is not guest memory) is
// not a missing route.
void everyAdvertisedRouteIsReachableByItsOwnMethod() {
    Fixture fixture;
    const std::string list{ControlChannel::routeList()};
    size_t advertised_count = 0;
    std::vector<std::string> missing;
    std::string rest = list;
    while (!rest.empty()) {
        const size_t comma = rest.find(',');
        std::string entry = rest.substr(0, comma);
        rest = comma == std::string::npos ? std::string{} : rest.substr(comma + 1);
        // The last entry is written "X and Y", so it is **two** advertised routes and both are
        // checked. It used to keep only the second of the two, which meant `POST /input` was
        // advertised and never reached a handler in this test -- the test passed while one of the
        // routes it claimed to cover was untested, and it read as coverage.
        const size_t conjunction = entry.rfind(" and ");
        std::string second;
        if (conjunction != std::string::npos) {
            second = entry.substr(conjunction + 5);
            entry = entry.substr(0, conjunction);
        }
        while (!entry.empty() && entry.front() == ' ') {
            entry.erase(entry.begin());
        }
        while (!entry.empty() && entry.back() == ' ') {
            entry.pop_back();
        }
        const size_t space = entry.find(' ');
        if (space == std::string::npos) {
            continue;
        }

        for (const std::string& advertised : {entry, second}) {
            if (advertised.empty()) {
                continue;
            }
            ++advertised_count;
            lucent::http::Request one;
            one.method = advertised.substr(0, advertised.find(' '));
            one.target = advertised.substr(advertised.find(' ') + 1);
            auto answered = fixture.channel.dispatch(one);
            if (answered.body.rfind("unknown route. This channel serves", 0) == 0) {
                // The status and the route's own name as the test built it, because "missing" with
                // no detail is a report that has to be re-run with a debugger in it. A route table
                // test that names the request it built and the answer it got can be read.
                missing.push_back(one.method + " " + one.target + " -> " +
                                  std::to_string(answered.status) + ", " +
                                  answered.body.substr(0, 40));
            }
        }
    }
    // The guard against a vacuous pass, stated as what it protects rather than as a number that
    // happens to be current. Every route the product serves is named here, so the loop above reads
    // a list that cannot silently shrink to nothing: the retired mechanism took fifteen routes with
    // it, and a floor that was set against the larger list failed on the smaller one for a reason
    // that had nothing to do with the routes.
    check::isTrue(
        advertised_count == 21,
        "every route the channel serves is in the list, so this test reads all of them: " +
            std::to_string(advertised_count) + " of 21");
    check::isTrue(
        missing.empty(), "every advertised route reaches a handler; these answer that they "
                         "do not exist: " +
                             [&missing] {
                                 std::string joined;
                                 for (const auto& route : missing) {
                                     joined += (joined.empty() ? "" : ", ") + route;
                                 }
                                 return joined;
                             }());
}

// A GET reports; it does not change anything. The routes that both read and arm
// -- GET /paint is the state, POST /paint arms it -- are the ones where a method
// check that was forgotten would show up as a diagnostic that installs a mod, and
// the arming would then happen in whichever run happened to ask. So: a GET of a
// route that has both forms must leave the mod off, and a GET of a POST-only
// route must be refused rather than quietly answered.
void aGetReportsAndDoesNotChangeAnything() {
    Fixture fixture;
    lucent::http::Request read;
    read.method = "GET";
    read.target = "/paint";
    auto state = fixture.channel.dispatch(read);
    check::isTrue(state.status == 200, "GET /paint answers with the state");
    check::isTrue(contains(state.body, "\"installed\":false"),
                  "and the mod is not installed by asking about it");

    // `/present` rather than a route that is gone: the point is a route that *exists* under one
    // method only, and the retired ones no longer answer for anything -- so testing the method
    // discipline on one of them would pass whether the discipline works or the route is simply
    // absent.
    lucent::http::Request postOnly;
    postOnly.method = "GET";
    postOnly.target = "/global-pose";
    auto refused = fixture.channel.dispatch(postOnly);
    check::isTrue(refused.body.rfind("unknown route. This channel serves", 0) == 0,
                  "a GET of a POST-only route is refused as unknown, not answered");
    check::isTrue(contains(refused.body, "POST /global-pose"),
                  "and the refusal still says the route exists for POST");

    // **And a route that was deleted stays deleted.** The host-side interpolation's routes are
    // gone, and a refusal that still advertised them would send a reader to a channel that does not
    // serve them -- so the refusal's own list is asserted not to name them.
    lucent::http::Request gone;
    gone.method = "GET";
    gone.target = "/transforms";
    auto retired = fixture.channel.dispatch(gone);
    check::isTrue(retired.body.rfind("unknown route. This channel serves", 0) == 0,
                  "a route the retired mechanism served is refused as unknown");
    check::isTrue(
        !contains(retired.body, "/transforms") && !contains(retired.body, "/replay") &&
            !contains(retired.body, "/substitution") && !contains(retired.body, "/blends"),
        "and the refusal's own list does not advertise it: " + retired.body.substr(0, 90));
}

} // namespace

namespace wiiuport::tests {

void runControlTests() {
    anIdleRuntimeReportsZerosRatherThanNothing();
    theCountersFollowTheRecorder();
    aChannelWithNoSetupScreenSaysSoRatherThanReportingAClosedOne();
    aShownSetupScreenReportsWhatItIsWaitingFor();
    aQuitWithNoHostRunningIsRefusedAndAQuitWithOneReachesIt();
    anUnstartedChannelIsNotRunning();
    aCensusTakesEntriesWithTheirFirstInstructionAndRefusesAnythingElse();
    anIdleCensusReportsNoEntriesRatherThanNothing();
    aMemoryReadNamesItsRangeAndIsBounded();
    everyAdvertisedRouteIsReachableByItsOwnMethod();
    aGetReportsAndDoesNotChangeAnything();
}

} // namespace wiiuport::tests
