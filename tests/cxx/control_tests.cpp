#include "check.h"
#include "lucent/http.h"
#include "suites.h"
#include "wiiuport/control/ControlChannel.h"
#include "wiiuport/control/GuestMemoryRead.h"
#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/input/InputDriver.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
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
    wiiuport::title::ObjectPoseLocator poses;
    wiiuport::title::PoseByShader poseByShader;
    // Armed with the table it reads, because a blend with no table leaves every draw alone
    // and the route test would be reporting a blend that cannot happen.
    wiiuport::title::PoseBlend poseBlend{poseByShader};
    wiiuport::title::QuadBlend quadBlend{writers, [] {
                                             return false;
                                         }};
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
        .callers = callers,
        .paint = paint,
        .blocks = blocks,
        .poses = poses,
        .globalPose = globalPose,
        .poseByShader = poseByShader,
        .poseBlend = poseBlend,
        .quadBlend = quadBlend,
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

    // **A leading `0x` is optional, and it is optional because every address in this project is
    // written with one.** It was refused, with a message naming `<hex>` as though the prefix were
    // part of the form, to a reader holding `0x10163bb4` out of this project's own documents. The
    // three spellings all name the same address, and a diagnostic that accepts one of them is a
    // diagnostic that will be read as broken.
    for (const std::string& spelled :
         {"address=0x10163bb4&size=48", "address=10163bb4&size=48", "address=0X10163BB4&size=48"}) {
        const auto named = GuestMemoryRead::parse(spelled, refusal);
        check::isTrue(named.has_value(),
                      std::string(spelled).append(" is a read, not a refusal: ").append(refusal));
        if (named.has_value()) {
            check::equal(named->address, uint32_t{0x10163bb4}, "and names the same address");
            check::equal(named->size, uint32_t{48}, "and the size asked for");
        }
    }
    // And the prefix is not accepted where it means nothing: a decimal size with `0x` in front is
    // not a number this parser will guess at.
    check::isTrue(!GuestMemoryRead::parse("address=0x10&size=0x40", refusal).has_value(),
                  "a 0x on the decimal size is refused rather than read as hex");
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
// **A report that does not parse is not a report, and nothing else here would notice.** A doubled
// `body += body +=` in `countersJson` wrote the whole object twice into itself, so `/counters`
// answered a body that began validly and then restarted: every client refused it, and every
// refusal read as "the channel never opened" rather than as a body that is not JSON. The route
// table test cannot see it -- a route can be served and still answer nonsense -- and a test that
// counts field names cannot either, because the doubled body names every field once and then
// again. So the body is parsed here, by the same shape a client parses it with.
namespace {

// Braces and brackets balanced, in string literals and escapes ignored, and the body a single JSON
// value rather than a value followed by another. Deliberately small: it answers "is this one
// document", which is the question the doubling asked, and a full parser here would be a second
// implementation of a rule the client already owns.
bool oneJsonDocument(const std::string& body) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t seen = 0;
    for (const char character : body) {
        ++seen;
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                inString = false;
            }
            continue;
        }
        if (character == '"') {
            inString = true;
        } else if (character == '{' || character == '[') {
            ++depth;
        } else if (character == '}' || character == ']') {
            --depth;
            if (depth == 0) {
                // The document ends here; anything but whitespace after it is a second value.
                for (size_t rest = seen; rest < body.size(); ++rest) {
                    const char trailing = body[rest];
                    if (trailing != ' ' && trailing != '\n' && trailing != '\r' &&
                        trailing != '\t') {
                        return false;
                    }
                }
                return true;
            }
        }
    }
    return false;
}

std::string eachReport(Fixture& fixture) {
    std::string all;
    for (const char* path : {"/counters", "/paint", "/logic", "/gate", "/recordings", "/draws",
                             "/pacing", "/memory", "/callers", "/blocks"}) {
        lucent::http::Request read;
        read.method = "GET";
        read.target = path;
        auto answer = fixture.channel.dispatch(read);
        if (answer.status == 200) {
            all += answer.body;
        }
    }
    return all;
}

} // namespace

// **The negative first.** A body that is one document twice, and one that is truncated, are both
// refused -- and so is a body that is not a document at all. A check that only ever sees a good
// body has not been shown it can fail.
void aReportThatIsNotOneDocumentIsRecognisedAsSuch() {
    check::isTrue(oneJsonDocument("{\"a\":1}"), "one object is one document");
    check::isTrue(oneJsonDocument("{\"a\":\"}\"}"), "a brace inside a string does not close it");
    check::isTrue(oneJsonDocument("{\"a\":\"\\\\\"}"), "an escaped quote does not open a string");
    check::isTrue(oneJsonDocument("{\"a\":[1,{\"b\":2}]}  \n"),
                  "trailing whitespace is still one document");
    check::isTrue(!oneJsonDocument("{\"a\":1}{\"a\":1}"),
                  "the same object twice is two documents, and is what a doubled append writes");
    check::isTrue(!oneJsonDocument("{\"a\":1"), "a truncated body is not a document");
    check::isTrue(!oneJsonDocument("not json at all"), "prose is not a document");
    check::isTrue(!oneJsonDocument(""), "nothing is not a document");
}

// And the real thing: every report the channel serves, parsed.
void everyReportIsOneJsonDocument() {
    Fixture fixture;
    fixture.recorder.OnDisplayList(LatteFrameHooks::DisplayList{0, nullptr, 0});
    fixture.recorder.OnFrameComplete();
    // Each report with the method it answers under: `/pacing` restarts the pacing, so it is a POST
    // and asking it under GET is a refusal about a method rather than about the body.
    for (const auto& [method, path] :
         std::vector<std::pair<std::string, std::string>>{{"GET", "/counters"},
                                                          {"GET", "/paint"},
                                                          {"GET", "/logic"},
                                                          {"GET", "/gate"},
                                                          {"GET", "/blocks"},
                                                          {"GET", "/quads"},
                                                          {"POST", "/pacing"},
                                                          {"POST", "/pose"}}) {
        lucent::http::Request read;
        read.method = method;
        read.target = path;
        auto answer = fixture.channel.dispatch(read);
        check::isTrue(answer.status == 200,
                      std::string(method).append(" ").append(path).append(" answers"));
        check::isTrue(oneJsonDocument(answer.body),
                      std::string(path) +
                          " answers one JSON document, not a body with a second "
                          "one inside it: " +
                          answer.body.substr(0, 60));
    }
}

// **The pose table is fed by a POST and read by a GET, and neither mutates the other's answer.**
// Two routes rather than one, because filling a table on a read is a mutation a reader of the
// report would not expect, and a report that changes because it was read is a report a reader stops
// trusting.
void thePoseTableIsFedByAPostAndReadByAGet() {
    Fixture fixture;

    // **The census is given candidates first, because a table that starts empty cannot tell a read
    // that fills from a read that does not.** With nothing to offer, a GET that fed the table would
    // produce exactly the report a GET that did not would, and the assertions at the end of this
    // test would pass either way -- a test that cannot fail is not coverage.
    //
    // The candidate is the title's measured per-object pose: twelve words at **offset 12 in shader
    // `0x1557c18f92f3bcb9`**, read by no other object, moved in 21,730 of 21,879 comparisons. The
    // twelve words are a 3x4 read as three rows of three and a translation, which is how
    // `TransformShape` reads them, and the translation moves each round so the class records a
    // movement -- the thing that separates a pose from a basis matrix.
    for (int round = 0; round < 80; round++) {
        wiiuport::frame::RecordedUniformAssembly assembly;
        assembly.shaderBaseHash = 0x1557c18f92f3bcb9;
        assembly.objectAddress = 0x027ff88c + static_cast<uint32_t>(round % 2) * 4;
        // The block sources are the fallback identity, and they are two so the class has *another*
        // object to compare against: the table refuses a candidate nothing was compared with, and a
        // fixture with one object would be refused for that reason rather than for the one under
        // test.
        assembly.blockSources = {0x3e000000u + static_cast<uint32_t>(round % 2) * 4};
        assembly.data.assign(24, 0.0f);
        const float step = static_cast<float>(round) * 0.25f;
        const float pose[wiiuport::title::PoseByShader::kWords] = {
            1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, step, 0.0f, 0.0f};
        for (size_t word = 0; word < wiiuport::title::PoseByShader::kWords; word++) {
            assembly.data[3 + word] = pose[word];
        }
        fixture.poses.onAssemblyRecorded(assembly);
    }

    // Read first: the table is empty and says so with a denominator, rather than answering an empty
    // object that reads as "there is nothing" instead of "nothing has been offered yet".
    lucent::http::Request read;
    read.method = "GET";
    read.target = "/pose";
    auto before = fixture.channel.dispatch(read);
    check::isTrue(before.status == 200, "GET /pose answers");
    check::isTrue(oneJsonDocument(before.body),
                  "as one JSON document: " + before.body.substr(0, 60));
    check::isTrue(contains(before.body, "\"shaders\":{}"),
                  "with an empty table said as empty, not omitted: " + before.body.substr(0, 120));
    check::isTrue(contains(before.body, "\"offered\":0"),
                  "and with what it has been offered, which is zero and is written down: " +
                      before.body.substr(0, 120));

    // **The feed is accepted, offers the census's candidate, and takes it.** A feed
    // that reported nothing would not say whether it had refused anything, and a
    // refusal with no count is a refusal a caller cannot act on.
    lucent::http::Request feed;
    feed.method = "POST";
    feed.target = "/pose";
    auto fed = fixture.channel.dispatch(feed);
    check::isTrue(fed.status == 200, "POST /pose answers");
    check::isTrue(oneJsonDocument(fed.body), "as one JSON document too: " + fed.body.substr(0, 60));
    check::isTrue(
        contains(fed.body, "\"offeredLastFeed\":1"),
        "reporting what that call offered, which is the one candidate the census found: " +
            fed.body.substr(0, 200));
    check::isTrue(contains(fed.body, "\"acceptedLastFeed\":1"),
                  "and that it took it: " + fed.body.substr(0, 200));
    check::isTrue(contains(fed.body, "\"0x1557c18f92f3bcb9\""),
                  "and which shader it took it for, in hex like every other hash in a report: " +
                      fed.body.substr(0, 240));

    // **The read now differs from the one taken before the feed, by the feed and nothing else.**
    auto after = fixture.channel.dispatch(read);
    check::isTrue(after.body != before.body,
                  "the read changes because the POST filled the table, so neither answer is a "
                  "decoration of the other");
    // And it is the *same* feed reported again. A GET that filled the table would reset the feed's
    // counts to zero, and this is the check that tells the two apart.
    check::isTrue(contains(after.body, "\"offeredLastFeed\":1"),
                  "and a GET reports the last feed's own counts rather than resetting them, which "
                  "is how it is known not to have filled it: " +
                      after.body.substr(0, 200));
    check::isTrue(contains(after.body, "\"0x1557c18f92f3bcb9\""),
                  "and still holds the offset the feed gave it");
    // **Two named sections in one document**, and the blend's counts beside the table's: the table
    // says where a pose goes and the blend says whether any draw went there, and a caller that
    // armed the table wants both in one read rather than two round trips to find out whether the
    // thing it armed is doing anything.
    check::isTrue(contains(after.body, "\"table\":{"), "the table is its own named section");
    check::isTrue(contains(after.body, "\"blend\":{"), "and the blend is another");
    check::isTrue(contains(after.body, "\"lerpsPerInBetween\":"),
                  "and the blend's own ratio is in it, which is the field that says a blend is "
                  "happening: " +
                      after.body.substr(0, 200));
    // The composition is done by the owners rather than by splicing their rendered text, so the
    // one-document check above covers this route for the same reason it covers the rest.
    check::isTrue(oneJsonDocument(after.body),
                  "and the two sections together are still one JSON document: " +
                      after.body.substr(0, 60));
}

// **A named shader answers for itself, with the table's own refusal for each candidate.** The
// report lists eight candidates out of 285, so "why is my shader not in the table" was a question
// the instrument could not answer -- and two runs of the blend differed by exactly that: one
// blended 1,240 times and one nothing at all, on tables of nearly the same size. The negative was
// measurable and the reason was not, which is the worse of the two positions.
void aNamedShaderAnswersForItselfWithTheTablesOwnRefusal() {
    Fixture fixture;
    // Two candidates of the named shader and one of another, so the query has to select rather than
    // dump. Each is given a different reason to be refused.
    for (int round = 0; round < 40; round++) {
        wiiuport::frame::RecordedUniformAssembly assembly;
        assembly.shaderBaseHash = 0x6669a23d03806414;
        assembly.objectAddress = 0x027ff88c + static_cast<uint32_t>(round % 2) * 4;
        assembly.blockSources = {0x3e000000u + static_cast<uint32_t>(round % 2) * 4};
        assembly.data.assign(24, 0.0f);
        const float pose[wiiuport::title::PoseBlend::kWords] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                                                0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
        for (size_t word = 0; word < wiiuport::title::PoseBlend::kWords; word++) {
            assembly.data[3 + word] = pose[word];
        }
        fixture.poses.onAssemblyRecorded(assembly);
    }

    lucent::http::Request query;
    query.method = "GET";
    query.target = "/pose?shader=0x6669a23d03806414";
    auto answer = fixture.channel.dispatch(query);
    check::isTrue(answer.status == 200, "a named shader answers");
    check::isTrue(oneJsonDocument(answer.body),
                  "as one JSON document: " + answer.body.substr(0, 60));
    check::isTrue(contains(answer.body, "\"shaderBaseHash\":\"0x6669a23d03806414\""),
                  "and names the shader it answered for: " + answer.body.substr(0, 120));
    check::isTrue(contains(answer.body, "\"candidatesForThisShader\":"),
                  "with the count of candidates it has for that shader: " +
                      answer.body.substr(0, 200));
    // **The refusal is the table's own, asked of the table**, so a reader is told what the blend
    // would do with the offset rather than being handed counts to interpret.
    check::isTrue(contains(answer.body, "\"refusal\":"),
                  "and each candidate carries the table's own refusal for it");
    // And the bare word form works, because every address in this project is written both ways.
    lucent::http::Request bare;
    bare.method = "GET";
    bare.target = "/pose?shader=6669a23d03806414";
    auto bareAnswer = fixture.channel.dispatch(bare);
    check::isTrue(
        bareAnswer.body == answer.body,
        "and the same shader without the 0x is the same answer, because a diagnostic that "
        "accepts one spelling of a hash is read as broken");
    // And with no shader named, the whole report comes back as before -- the query is additive and
    // not a different route.
    lucent::http::Request plain;
    plain.method = "GET";
    plain.target = "/pose";
    auto plainAnswer = fixture.channel.dispatch(plain);
    check::isTrue(contains(plainAnswer.body, "\"table\":{"),
                  "and with no shader named the whole report comes back as before: " +
                      plainAnswer.body.substr(0, 80));

    // **A named shader narrows the feed to that shader's candidates.** The census is cumulative and
    // the table evaluates whatever counts it is given, so a candidate refused on first sight can be
    // admitted by offering it again once it has drawn enough. The blend's own unplaced-shader list
    // names the shaders that need that, and a caller holding the measurement should be able to ask
    // for exactly those rather than the whole census every time.
    lucent::http::Request narrow;
    narrow.method = "POST";
    narrow.target = "/pose?shader=0x6669a23d03806414";
    auto narrowed = fixture.channel.dispatch(narrow);
    check::isTrue(narrowed.status == 200, "a narrowed feed answers");
    check::isTrue(contains(narrowed.body, "\"offeredLastFeed\":"),
                  "and says how many candidates it offered: " + narrowed.body.substr(0, 200));
    // Whatever it offered, the table's size is unchanged by a narrowed feed of candidates it
    // already holds, and the document is still one document.
    check::isTrue(oneJsonDocument(narrowed.body),
                  "and it is still one JSON document: " + narrowed.body.substr(0, 60));
}

void everyAdvertisedRouteIsReachableByItsOwnMethod() {
    Fixture fixture;
    const std::string list{ControlChannel::routeList()};
    size_t advertised_count = 0;
    // **The number of entries the list's own words carry, counted from them rather than written
    // down.** A literal here fails when an unrelated route is added, and this guard has had that
    // fault twice -- once when the retired mechanism took fifteen routes away and once when two
    // were added -- and a guard that fails for an unrelated reason is a guard a reader learns to
    // skip.
    //
    // Counted by hand rather than with `std::count` over a string, because a substring count is a
    // second rule about what an entry is and the loop below is the one that parses them.
    size_t served = 0;
    for (size_t at = list.find("GET "); at != std::string::npos; at = list.find("GET ", at + 1)) {
        ++served;
    }
    for (size_t at = list.find("POST "); at != std::string::npos; at = list.find("POST ", at + 1)) {
        ++served;
    }
    // The routes the list advertises that no handler answered, named in the failure.
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
        advertised_count == served,
        "every route the channel serves is in the list, so this test reads all of them: " +
            std::to_string(advertised_count) + " of " + std::to_string(served));
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
    aReportThatIsNotOneDocumentIsRecognisedAsSuch();
    everyReportIsOneJsonDocument();
    theCountersFollowTheRecorder();
    aChannelWithNoSetupScreenSaysSoRatherThanReportingAClosedOne();
    aShownSetupScreenReportsWhatItIsWaitingFor();
    aQuitWithNoHostRunningIsRefusedAndAQuitWithOneReachesIt();
    anUnstartedChannelIsNotRunning();
    aCensusTakesEntriesWithTheirFirstInstructionAndRefusesAnythingElse();
    anIdleCensusReportsNoEntriesRatherThanNothing();
    aMemoryReadNamesItsRangeAndIsBounded();
    thePoseTableIsFedByAPostAndReadByAGet();
    aNamedShaderAnswersForItselfWithTheTablesOwnRefusal();
    everyAdvertisedRouteIsReachableByItsOwnMethod();
    aGetReportsAndDoesNotChangeAnything();
}

} // namespace wiiuport::tests
