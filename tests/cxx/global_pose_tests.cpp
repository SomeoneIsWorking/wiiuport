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
std::map<uint32_t, uint32_t> blankRange() {
    std::map<uint32_t, uint32_t> words;
    for (size_t word = 0; word < GlobalPoseCensus::kWords; word++) {
        words[GlobalPoseCensus::kStart + 4 * word] = floatBits(0.0f);
    }
    return words;
}

// A 3x4 at `offsetWords` into the range: three perpendicular rows of the given scale, and a
// translation. `translationDelta` is how far the translation is between the two snapshots, and zero
// means the same value in both -- the static case.
void put3x4(std::map<uint32_t, uint32_t>& words, size_t offsetWords, float rotation, float scale,
            float translationDelta) {
    const float s = std::sin(rotation);
    const float c = std::cos(rotation);
    const float values[12] = {
        c * scale, s * scale, 0.0f, -s * scale, c * scale,
        0.0f,      0.0f,      0.0f, scale,      5.0f + translationDelta,
        -2.0f,     8.0f,
    };
    for (size_t word = 0; word < 12; word++) {
        words[GlobalPoseCensus::kStart + 4 * (offsetWords + word)] = floatBits(values[word]);
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
    return body.substr(start, end - start);
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

void wiiuport::tests::runGlobalPoseCensusTests() {
    {
        // **A 3x4 that moved between the two readings is found, at its own address.** The
        // denominator is in the report and the address is the one the test wrote to -- a scan that
        // found a transform somewhere would be a scan that found the filler.
        auto before = blankRange();
        auto after = blankRange();
        const size_t offsetWords = 4096;
        // The same 3x4 in both, with the translation moved: a pose that advanced, which is what a
        // camera between two frames is.
        put3x4(before, offsetWords, 0.7f, 2.0f, 0.0f);
        put3x4(after, offsetWords, 0.7f, 2.0f, 0.25f);
        g_first = &before;
        g_second = &after;
        g_reads = 0;
        GlobalPoseCensus census(&readWords, &frame);
        std::string refusal;
        const size_t named = census.scan(refusal);
        const std::string body = census.json();
        g_first = nullptr;
        g_second = nullptr;
        check::isTrue(refusal.empty(), "a scan that read its range refuses nothing: " + refusal);
        check::isTrue(named == 1, "a transform that moved is named exactly once, and once is "
                                  "the honest count for one written: " +
                                      std::to_string(named));
        check::isTrue(mentions(body, hexOf(GlobalPoseCensus::kStart + 4 * offsetWords)),
                      "and the report names the address it was written to, so the hit is the "
                      "written one and not the filler");
        check::isTrue(field(body, "windowsTestedLastScan") ==
                          std::to_string(GlobalPoseCensus::kWords -
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
        auto before = blankRange();
        auto after = blankRange();
        const size_t offsetWords = 8192;
        put3x4(before, offsetWords, 0.3f, 1.0f, 0.0f);
        put3x4(after, offsetWords, 0.3f, 1.0f, 0.0f);
        g_first = &before;
        g_second = &after;
        g_reads = 0;
        GlobalPoseCensus census(&readWords, &frame);
        std::string refusal;
        const size_t named = census.scan(refusal);
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
        const size_t named = census.scan(refusal);
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
        auto before = blankRange();
        auto after = blankRange();
        put3x4(before, 1024, 0.5f, 1.0f, 0.0f);
        g_first = &before;
        g_second = &after;
        g_reads = 0;
        GlobalPoseCensus census(&readWords, []() {
            return 42;
        });
        std::string refusal;
        const size_t named = census.scan(refusal);
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
        // **The range is the title's own data and bss, contiguous, and its size is reported.**
        // Asserted rather than trusted: a scan that quietly covered less than it claimed would
        // make every denominator in the report a smaller lie.
        check::isTrue(GlobalPoseCensus::kEnd > GlobalPoseCensus::kStart, "the range is not empty");
        check::isTrue(GlobalPoseCensus::kWords * 4 ==
                          GlobalPoseCensus::kEnd - GlobalPoseCensus::kStart,
                      "and its word count is the range divided by four, with nothing dropped");
        GlobalPoseCensus census(&readWords, &frame);
        check::isTrue(field(census.json(), "rangeBytes") ==
                          std::to_string(GlobalPoseCensus::kEnd - GlobalPoseCensus::kStart),
                      "and the report carries the range's own size: " +
                          field(census.json(), "rangeBytes"));
    }
}
