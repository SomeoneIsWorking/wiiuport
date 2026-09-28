/// The data-area scan: two snapshots a frame apart, and what it does with the difference.

#include "check.h"
#include "suites.h"
#include "wiiuport/title/GlobalPoseCensus.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {

using wiiuport::title::GlobalPoseCensus;

// The range every case scans. **Not a constant but a call**, so the test exercises the same range
// the product builds rather than one written out here that could drift from it.
const GlobalPoseCensus::Range& RANGE() {
    static const GlobalPoseCensus::Range range = GlobalPoseCensus::data();
    return range;
}

// **The guest, as two snapshots rather than one.** The scan reads the range twice and compares, so
// a reader that answered both reads from the same map would report that nothing ever moved -- and
// would then be testing the double rather than the scan. So there are two maps and the reader
// alternates between them on each full-range read, which is what a frame boundary looks like from
// inside a reader.
std::map<uint32_t, uint32_t>* g_first = nullptr;
std::map<uint32_t, uint32_t>* g_second = nullptr;
size_t g_reads = 0;
constexpr uint32_t kFullRead = 1024; // the census reads the whole range in one call

bool readWords(uint32_t address, uint32_t* values, uint32_t count) {
    const bool full = count > kFullRead;
    if (full) {
        g_reads++;
    }
    const std::map<uint32_t, uint32_t>* words = (full && (g_reads % 2) == 0) ? g_second : g_first;
    if (words == nullptr) {
        return false;
    }
    for (uint32_t word = 0; word < count; word++) {
        auto found = words->find(address + 4 * word);
        if (found == words->end()) {
            return false;
        }
        values[word] = found->second;
    }
    return true;
}

// The frame counter, which the scan waits on and **which advances on every read**. A counter that
// stood still would make every scan refuse, and the refusal has its own test below with its own
// counter that does exactly that -- so the two are told apart rather than one standing in for both.
std::atomic<uint64_t> g_frame{0};

uint64_t frame() {
    return g_frame.fetch_add(1) + 1;
}

uint32_t floatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// The whole declared range as zeroes, so an offset the test does not write cannot satisfy the
// predicate. A partly-filled range would let the assertion be about the filler.
std::map<uint32_t, uint32_t> blankRange(const GlobalPoseCensus::Range& range) {
    std::map<uint32_t, uint32_t> words;
    for (size_t word = 0; word < (range.end - range.start) / 4; word++) {
        words[range.start + 4 * word] = floatBits(0.0f);
    }
    return words;
}

// A 3x4 at `offsetWords` into the range: three perpendicular rows of the given scale, and a
// translation. `translationDelta` is how far the translation is between the two snapshots, and zero
// means the same value in both -- the static case.
void put3x4(std::map<uint32_t, uint32_t>& words, uint32_t start, size_t offsetWords, float rotation,
            float scale, float translationDelta) {
    const float s = std::sin(rotation);
    const float c = std::cos(rotation);
    const float values[12] = {
        c * scale, s * scale, 0.0f, -s * scale, c * scale,
        0.0f,      0.0f,      0.0f, scale,      5.0f + translationDelta,
        -2.0f,     8.0f,
    };
    for (size_t word = 0; word < 12; word++) {
        words[start + 4 * (offsetWords + word)] = floatBits(values[word]);
    }
}

std::string field(const std::string& body, const std::string& name) {
    const size_t at = body.find("\"" + name + "\":");
    if (at == std::string::npos) {
        return "";
    }
    const size_t start = at + name.size() + 3;
    size_t end = start;
    while (end < body.size() && body[end] != ',' && body[end] != '}') {
        end++;
    }
    std::string value = body.substr(start, end - start);
    // **A JSON string's value comes back with its quotes, and a comparison against a C++ literal
    // that forgets them fails for a reason no failure message shows.** So they are stripped here,
    // once, and every caller compares against the bare text. This cost an afternoon once already:
    // a field that read exactly as expected and an assertion that was false.
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool mentions(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string hexOf(uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08x", value);
    return buffer;
}

} // namespace

// The one string test every file in this suite needs, written here rather than in a shared header:
// two files that each carry their own is a helper this project has already had to fix twice.
bool carries(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

// **A range the caller names, refused by reason rather than scanned anyway.** The two ranges this
// class holds in a table have fixed addresses; the uniform block a draw sources does not, because
// it is wherever the title's binder put it. The measurement that unblocked the blend needs exactly
// those, so a caller may name one -- and every way of naming it badly has to be refused with the
// value that caused it, because "bad request" sends a reader to look at the wrong half.
void aNamedRangeIsAcceptedAsAskedAndRefusedByReasonOtherwise() {
    // The positive, in the form the channel's own query carries it: `0x` in front is optional and
    // the digits are case-insensitive, because a tool prints an address in whichever case it has.
    for (const std::string& start : {"0x47eee100", "47EEE100", "47eee100"}) {
        const auto range = GlobalPoseCensus::namedRange(start, "768");
        check::isTrue(range.refusal.empty(), "start=" + start + " is accepted: " + range.refusal);
        check::equal(range.askedStart, 0x47eee100u, "and names the address it asked for");
        check::equal(range.askedEnd, 0x47eee400u, "and the end the size makes");
        check::equal(range.range.end - range.range.start, 768u,
                     "so the range is the size asked for");
    }

    // And the negative, each with its own reason. A refusal that named a different fault would be
    // a refusal that sends the reader to the wrong field, so the reason is asserted, not the fact
    // of refusal.
    struct Case {
        const char* start;
        const char* bytes;
        const char* because;
    };

    for (const Case& one : std::vector<Case>{
             {"0x47eee101", "768", "not four-aligned"},
             {"0x47eee100", "0", "scans nothing"},
             {"0x47eee100", "8388608", "a named range may be"},
             // Within the cap and still past the end, so this case reaches the wrap check rather
             // than the cap that is tested on the line above. Two faults at once would be reported
             // as one, and the reader would be sent to the wrong bound.
             {"0xffffff00", "768", "past the end"},
             {"nonsense", "768", "not a hexadecimal guest address"},
             {"0x47eee100", "seven", "not a decimal count"},
             {"", "768", "not a hexadecimal guest address"},
         }) {
        const auto range = GlobalPoseCensus::namedRange(one.start, one.bytes);
        check::isTrue(!range.refusal.empty(),
                      std::string("start=") + one.start + " bytes=" + one.bytes + " is refused");
        check::isTrue(carries(range.refusal, one.because),
                      std::string("and says why: ") + one.because + " -- got: " + range.refusal);
        check::equal(range.askedEnd, 0u, "and names no range, so nothing is scanned");
    }
    // The cap itself, named rather than implied: a caller that asks for exactly the cap is served,
    // and one more byte is refused. A bound nobody can state is a bound that moves.
    check::isTrue(GlobalPoseCensus::namedRange("0x10000000", "4194304").refusal.empty(),
                  "a range of exactly the cap is served");
    check::isTrue(!GlobalPoseCensus::namedRange("0x10000000", "4194305").refusal.empty(),
                  "and one byte more is refused");
}

void wiiuport::tests::runGlobalPoseCensusTests() {
    aNamedRangeIsAcceptedAsAskedAndRefusedByReasonOtherwise();
    {
        // **A 3x4 that moved between the two readings is found, at its own address.** The
        // denominator is in the report and the address is the one the test wrote to -- a scan that
        // found a transform somewhere would be a scan that found the filler.
        auto before = blankRange(RANGE());
        auto after = blankRange(RANGE());
        const size_t offsetWords = 4096;
        // The same 3x4 in both, with the translation moved: a pose that advanced, which is what a
        // camera between two frames is.
        put3x4(before, RANGE().start, offsetWords, 0.7f, 2.0f, 0.0f);
        put3x4(after, RANGE().start, offsetWords, 0.7f, 2.0f, 0.25f);
        g_first = &before;
        g_second = &after;
        g_reads = 0;
        GlobalPoseCensus census(&readWords, &frame);
        std::string refusal;
        const size_t named = census.scan(refusal, RANGE());
        const std::string body = census.json();
        g_first = nullptr;
        g_second = nullptr;
        check::isTrue(refusal.empty(), "a scan that read its range refuses nothing: " + refusal);
        check::isTrue(named == 1, "a transform that moved is named exactly once, and once is "
                                  "the honest count for one written: " +
                                      std::to_string(named));
        check::isTrue(mentions(body, hexOf(RANGE().start + 4 * offsetWords)),
                      "and the report names the address it was written to, so the hit is the "
                      "written one and not the filler");
        check::isTrue(field(body, "windowsTestedLastScan") ==
                          std::to_string((RANGE().end - RANGE().start) / 4 -
                                         wiiuport::title::TransformShape::kWords + 1),
                      "and the denominator is every window in the range: " +
                          field(body, "windowsTestedLastScan"));
        check::isTrue(field(body, "scans") == "1", "one scan, counted: " + field(body, "scans"));
        // The invariant is the one that catches a field nobody assigned: the moved windows are the
        // kept poses plus the ones collapsed into them. Asserting the two counts were *equal*
        // would have been asserting the collapse does not happen -- and the first version of this
        // test did exactly that, against a report that said "0 moved, 1 pose kept".
        const size_t movedCount =
            std::strtoull(field(body, "windowsAffineAndMoved").c_str(), nullptr, 10);
        const size_t keptCount = std::strtoull(field(body, "posesKept").c_str(), nullptr, 10);
        const size_t collapsedCount =
            std::strtoull(field(body, "windowsCollapsedIntoThosePoses").c_str(), nullptr, 10);
        check::isTrue(movedCount == keptCount + collapsedCount,
                      "and the moved windows are exactly the kept poses plus the ones collapsed "
                      "into them, so no count can be left unassigned: " +
                          std::to_string(movedCount) + " = " + std::to_string(keptCount) + " + " +
                          std::to_string(collapsedCount));
        check::isTrue(collapsedCount > 0,
                      "and the collapse is reported rather than silent -- one 3x4 satisfies the "
                      "class at several windows, and " +
                          std::to_string(collapsedCount) + " of them were its own");
        check::isTrue(mentions(body, "\"scaled\""),
                      "and the class it matched is named, so a scaled pose is not reported as a "
                      "rigid one and a reader can tell which test accepted it");
    }
    {
        // **A transform that did not move is not named.** The same 3x4, identical in both
        // readings. This is the negative that matters most: a data area is full of basis matrices,
        // normals and identity blocks, and a scan that named them would be naming the whole `.data`
        // section.
        auto before = blankRange(RANGE());
        auto after = blankRange(RANGE());
        const size_t offsetWords = 8192;
        put3x4(before, RANGE().start, offsetWords, 0.3f, 1.0f, 0.0f);
        put3x4(after, RANGE().start, offsetWords, 0.3f, 1.0f, 0.0f);
        g_first = &before;
        g_second = &after;
        g_reads = 0;
        GlobalPoseCensus census(&readWords, &frame);
        std::string refusal;
        const size_t named = census.scan(refusal, RANGE());
        const std::string body = census.json();
        g_first = nullptr;
        g_second = nullptr;
        check::isTrue(named == 0,
                      "a static transform is not a pose, and this is the bar that can fail: " +
                          std::to_string(named) + " named of " +
                          field(body, "windowsTestedLastScan") + " windows");
        // The class count is a count of *windows*, and one 3x4 satisfies the class at several
        // of them -- four, measured, because the scan steps one word at a time. So the assertion
        // is that the class test saw it at all, and the movement test is what rejected it.
        check::isTrue(std::strtoull(field(body, "windowsInAffineClass").c_str(), nullptr, 10) >= 1,
                      "and the class test did see it, so the zero is the movement test and not "
                      "a predicate that found nothing: " +
                          field(body, "windowsInAffineClass"));
        check::isTrue(field(body, "windowsAffineAndMoved") == "0",
                      "and the report says so in the words a reader can count: " +
                          field(body, "windowsAffineAndMoved"));
        check::isTrue(field(body, "posesKept") == "0",
                      "and no pose is kept, which is the count a reader compares with another "
                      "run: " +
                          field(body, "posesKept"));
    }
    {
        // **A reader that refuses is a refusal, not a zero.** The point of a scan over three
        // megabytes is that a run which could not read its range says it read nothing; a zero here
        // would read as "the data area holds no moving transform", which is the opposite.
        g_first = nullptr;
        g_second = nullptr;
        g_reads = 0;
        GlobalPoseCensus census(&readWords, &frame);
        std::string refusal;
        const size_t named = census.scan(refusal, RANGE());
        const std::string body = census.json();
        check::isTrue(named == 0,
                      "a scan that read nothing names nothing: " + std::to_string(named));
        check::isTrue(!refusal.empty(), "and refuses, rather than reporting a count: " + refusal);
        check::isTrue(mentions(refusal, "read nothing"),
                      "with the reason in its own words, so the refusal cannot be read as a "
                      "result: " +
                          refusal);
        check::isTrue(field(body, "scans") == "0",
                      "and the scan count stays at zero, so a refused scan is not a scan: " +
                          field(body, "scans"));
        check::isTrue(field(body, "scansRefused") == "1",
                      "while the refusal is counted: " + field(body, "scansRefused"));
    }
    {
        // **A frame counter that stands still is a refusal too, and a different one.** Two readings
        // at the same instant cannot measure movement, and reporting the static transforms they
        // both hold as "not moved" would answer a question nobody asked.
        auto before = blankRange(RANGE());
        auto after = blankRange(RANGE());
        put3x4(before, RANGE().start, 1024, 0.5f, 1.0f, 0.0f);
        g_first = &before;
        g_second = &after;
        g_reads = 0;
        GlobalPoseCensus census(&readWords, []() {
            return 42;
        });
        std::string refusal;
        const size_t named = census.scan(refusal, RANGE());
        const std::string body = census.json();
        g_first = nullptr;
        g_second = nullptr;
        check::isTrue(named == 0, "and it names nothing: " + std::to_string(named));
        check::isTrue(mentions(refusal, "same instant"),
                      "saying the two readings were the same instant, which is the actual fault: " +
                          refusal);
        check::isTrue(field(body, "scans") == "0",
                      "and is not counted as a scan: " + field(body, "scans"));
    }
    {
        // **Both ranges are named, and the two facts about them that a reader needs are asserted
        // rather than trusted.** The first was measured to hold nothing that moves with the camera
        // moving, and the second is where a GX2 uniform block lives -- so the scan can be pointed
        // at either, and a scan that cannot be pointed elsewhere cannot answer the next question.
        //
        // A name the code does not have is a refusal rather than a default: a caller asking for a
        // range that does not exist would otherwise get another range's answer and not know.
        check::isTrue(GlobalPoseCensus::data().end > GlobalPoseCensus::data().start,
                      "the data range is not empty");
        check::isTrue(GlobalPoseCensus::gpuUniformBlocks().end >
                          GlobalPoseCensus::gpuUniformBlocks().start,
                      "and neither is the uniform-block range");
        check::isTrue(GlobalPoseCensus::rangeByName("data").start == GlobalPoseCensus::data().start,
                      "a name resolves to the range it names");
        check::isTrue(GlobalPoseCensus::rangeByName("gpu-uniform-blocks").start ==
                          GlobalPoseCensus::gpuUniformBlocks().start,
                      "and so does the other one");
        check::isTrue(GlobalPoseCensus::rangeByName("nowhere").end == 0,
                      "while a name that names neither is an empty range, which the scan refuses "
                      "rather than answering with another range's numbers");

        // And the refusal is the refusal, through the census rather than by inspection.
        g_first = nullptr;
        g_second = nullptr;
        g_reads = 0;
        GlobalPoseCensus census(&readWords, &frame);
        std::string refusal;
        const size_t named = census.scan(refusal, GlobalPoseCensus::rangeByName("nowhere"));
        check::isTrue(named == 0 && mentions(refusal, "no such range"),
                      "and a scan pointed at it refuses in those words: " + refusal);

        // **A refused scan reports no range at all, and that is the honest value.** Reporting the
        // default range's size would say "I read 3,072,264 bytes" about a scan that read none, and
        // every denominator in the report would then be a lie with a number attached.
        check::isTrue(field(census.json(), "rangeBytes") == "0",
                      "and a census whose scan was refused reports a range of zero rather than the "
                      "default's: " +
                          field(census.json(), "rangeBytes"));
        check::isTrue(field(census.json(), "rangeName") == "(none scanned)",
                      "and names no range rather than naming the default: " +
                          field(census.json(), "rangeName"));

        // A scan that *did* read reports the range's own size, so a bound that was too small is
        // visible rather than assumed.
        auto before = blankRange(RANGE());
        auto after = blankRange(RANGE());
        g_first = &before;
        g_second = &after;
        g_reads = 0;
        std::string readRefusal;
        census.scan(readRefusal, RANGE());
        g_first = nullptr;
        g_second = nullptr;
        check::isTrue(field(census.json(), "rangeBytes") ==
                          std::to_string(RANGE().end - RANGE().start),
                      "and one that read reports the range's own size: " +
                          field(census.json(), "rangeBytes"));
        check::isTrue(field(census.json(), "rangeName") == "the title's .data and .bss",
                      "under the name it was asked for: got [" + field(census.json(), "rangeName") +
                          "]");
    }
}
