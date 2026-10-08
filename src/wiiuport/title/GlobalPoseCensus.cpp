#include "wiiuport/title/GlobalPoseCensus.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace wiiuport::title {

namespace {

float asFloat(uint32_t word) {
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(word), "a float is four bytes here");
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

std::string hexValue(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return {text};
}

} // namespace

GlobalPoseCensus::Range GlobalPoseCensus::data() {
    return kTitleRange;
}

GlobalPoseCensus::Range GlobalPoseCensus::gpuUniformBlocks() {
    return kGpuUniformRange;
}

// A hexadecimal address as a caller writes it, and whether it was one at all: `0x` in front is
// optional, digits are case-insensitive, and a byte count is decimal because a size is a size.
namespace {

bool parseHexWord(const std::string& text, uint32_t& out) {
    std::string_view body = text;
    if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) {
        body.remove_prefix(2);
    }
    if (body.empty() || body.size() > 8) {
        return false;
    }
    uint32_t value = 0;
    for (const char& digit : body) {
        uint32_t nibble = 0;
        if (digit >= '0' && digit <= '9') {
            nibble = static_cast<uint32_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            nibble = static_cast<uint32_t>(digit - 'a') + 10u;
        } else if (digit >= 'A' && digit <= 'F') {
            nibble = static_cast<uint32_t>(digit - 'A') + 10u;
        } else {
            return false;
        }
        value = (value << 4) | nibble;
    }
    out = value;
    return true;
}

bool parseDecimal(const std::string& text, uint32_t& out) {
    if (text.empty() || text.size() > 10) {
        return false;
    }
    uint32_t value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return false;
        }
        value = (value * 10u) + static_cast<uint32_t>(digit - '0');
    }
    out = value;
    return true;
}

} // namespace

GlobalPoseCensus::NamedRange GlobalPoseCensus::namedRange(const std::string& start,
                                                          const std::string& bytes) {
    NamedRange answer;
    uint32_t begin = 0;
    if (!parseHexWord(start, begin)) {
        answer.refusal = "start=" + start +
                         " is not a hexadecimal guest address; a named range is asked for as "
                         "start=<hex>&bytes=<decimal>";
        return answer;
    }
    uint32_t size = 0;
    if (!parseDecimal(bytes, size)) {
        answer.refusal = "bytes=" + bytes + " is not a decimal count of bytes";
        return answer;
    }
    // **Every refusal names the value that caused it.** A scan refused for an unaligned start and a
    // scan refused for a size over the cap are different faults, and a single "bad request" would
    // send a reader to look at the wrong half of the request.
    if ((begin & 0x3u) != 0) {
        answer.refusal = "start=" + start +
                         " is not four-aligned, and a transform-shaped window "
                         "can only begin on a four-byte boundary";
        return answer;
    }
    if (size == 0) {
        answer.refusal =
            "bytes=0 scans nothing, and a scan of nothing reports a zero that reads as "
            "a finding";
        return answer;
    }
    if (size > kMaxRangeBytes) {
        answer.refusal = "bytes=" + bytes + " is " + std::to_string(size) + ", over the " +
                         std::to_string(kMaxRangeBytes) +
                         " a named range may be; a wider range is "
                         "a region rather than a block";
        return answer;
    }
    const uint64_t end = static_cast<uint64_t>(begin) + size;
    if (end > 0x100000000ull) {
        answer.refusal = "start=" + start + " with bytes=" + bytes +
                         " runs past the end of the "
                         "guest's address space";
        return answer;
    }
    answer.askedStart = begin;
    answer.askedEnd = static_cast<uint32_t>(end);
    answer.range = {"named", "the range the caller named", begin, answer.askedEnd};
    return answer;
}

const GlobalPoseCensus::Range& GlobalPoseCensus::rangeByName(const std::string& name) {
    const Range& title = kTitleRange;
    const Range& gpu = kGpuUniformRange;
    const Range& unknown = kNoSuchRange;
    if (name == title.token) {
        return title;
    }
    if (name == gpu.token) {
        return gpu;
    }
    return unknown;
}

size_t GlobalPoseCensus::scan(std::string& refusal, const Range& range) {
    refusal.clear();
    if (range.end <= range.start) {
        refusal = std::string("no such range: ") + range.token;
        m_lastRefusal = refusal;
        m_refusedScans++;
        return 0;
    }
    const uint32_t start = range.start;
    uint32_t words = (range.end - range.start) / 4;
    m_rangeName = range.label;
    m_rangeStart = start;
    m_rangeEnd = range.end;
    const uint64_t firstFrame = m_frame ? m_frame() : 0;

    // Snapshot one, then wait for the frame counter to move before snapshot two. **A frame apart is
    // the whole point**: two readings taken back to back would report a transform that has not
    // moved as one that has, and that mistake is the whole difference between finding the camera
    // and finding every basis matrix in the data area.
    std::vector<uint32_t> before(words);
    std::vector<uint32_t> after(words);
    if (!m_readWords(start, before.data(), words)) {
        refusal = "the guest would not give up " + std::to_string(words) + " words at " +
                  hexValue(start) + ", so the scan read nothing";
        m_lastRefusal = refusal;
        m_refusedScans++;
        return 0;
    }
    bool advanced = false;
    for (uint32_t step = 0; step < kWaitSteps; step++) {
        if (m_frame && m_frame() != firstFrame) {
            advanced = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kWaitMs / kWaitSteps));
    }
    if (!m_readWords(start, after.data(), words)) {
        refusal = "the guest would not give up the second reading at " + hexValue(start);
        m_lastRefusal = refusal;
        m_refusedScans++;
        return 0;
    }
    if (!advanced) {
        // Named rather than folded into the counts, because two readings at the same frame number
        // would find every static transform moving not at all, and report that as a result.
        refusal = "the frame counter stood at " + std::to_string(firstFrame) + " through " +
                  std::to_string(kWaitMs) + "ms, so the two readings are the same instant and no " +
                  "movement could be measured";
        m_lastRefusal = refusal;
        m_refusedScans++;
        return 0;
    }

    m_hits.clear();
    size_t tested = 0;
    size_t classified = 0;
    size_t rigid = 0;
    size_t moved = 0;
    for (size_t word = 0; word + TransformShape::kWords <= words; word++) {
        tested++;
        const float* first = reinterpret_cast<const float*>(before.data() + word);
        const float* second = reinterpret_cast<const float*>(after.data() + word);
        if (!TransformShape::isAffine(first)) {
            continue;
        }
        classified++;
        // **Both readings must classify.** A transform that appears between them is something being
        // constructed rather than a value that is being updated, and the two are different facts
        // with different owners.
        if (!TransformShape::isAffine(second)) {
            continue;
        }
        if (!TransformShape::moved(first, second)) {
            continue;
        }
        moved++;
        Hit hit;
        hit.offset = static_cast<uint32_t>(word * 4);
        hit.address = start + hit.offset;
        hit.biggestDelta = TransformShape::deltaOf(first, second);
        hit.scale = TransformShape::scaleOf(first);
        hit.rigid = TransformShape::isRigid(first);
        if (hit.rigid) {
            rigid++;
        }
        if (m_hits.size() < kKeep) {
            m_hits.push_back(hit);
        }
    }
    // The most-changed first, so the cap keeps the ones that moved most rather than the first ones
    // found -- a scan reporting its lowest addresses is a scan reporting its ordering.
    std::sort(m_hits.begin(), m_hits.end(), [](const Hit& a, const Hit& b) {
        return a.biggestDelta > b.biggestDelta;
    });
    // **Overlapping windows are one pose, and the report counts poses.**
    //
    // A scan steps one word at a time, so a 3x4 also matches at every offset inside it where the
    // twelve words happen to satisfy the test -- measured on a single 3x4 written into an otherwise
    // zero range: **4 windows named where one 3x4 was written.** Those are four views of one value,
    // and a report whose count is a property of the step size rather than of the data cannot be
    // compared with anything.
    //
    // So a hit is dropped when its address is within one 3x4 of a hit already kept. The kept hit is
    // the one that moved most, because the sort put it first, so a real pose is kept and its own
    // shifted fragments are not -- and the count becomes the number of distinct poses in the data
    // area rather than the number of windows that happened to satisfy a test.
    // **The rule is `TransformShape::sameShapeAs`,** which `ObjectPoseLocator` also asks. It used
    // to be a lambda here and a second copy of the same arithmetic there, and two copies of a rule
    // that decides how many poses a report claims are two numbers a reader cannot compare.
    std::vector<uint32_t> keptAddresses;
    std::vector<Hit> kept;
    for (const Hit& hit : m_hits) {
        if (!TransformShape::sameShapeAs(keptAddresses, hit.address)) {
            keptAddresses.push_back(hit.address);
            kept.push_back(hit);
        }
        if (kept.size() >= kReported) {
            break;
        }
    }
    m_hits = std::move(kept);
    m_collapsed = moved - m_hits.size();
    // Both counts, and the report carries each of them. A run that said "0 moved, 1 pose kept"
    // because one of the two was never assigned is a report that contradicts itself, and a
    // self-contradicting report is the kind this project has been bitten by enough to check.
    m_moved = moved;
    m_scans++;
    m_wordsTested = tested;
    m_classified = classified;
    m_rigidHits = rigid;
    return m_hits.size();
}

std::string GlobalPoseCensus::json() const {
    JsonBody body;
    body.string("rangeName", m_rangeName.empty() ? "(none scanned)" : m_rangeName);
    body.string("range", hexValue(m_rangeStart) + "-" + hexValue(m_rangeEnd));
    body.number("rangeBytes", m_rangeEnd - m_rangeStart);
    body.number("scans", m_scans);
    body.number("windowsTestedLastScan", m_wordsTested);
    body.number("windowsInAffineClass", m_classified);
    body.number("windowsAffineAndMoved", m_moved);
    body.number("posesKept", m_hits.size());
    body.number("windowsCollapsedIntoThosePoses", m_collapsed);
    body.number("windowsRigidAndMoved", m_rigidHits);
    body.number("motionEpsilon", static_cast<uint64_t>(TransformShape::kMotionEpsilon * 1e9));
    body.number("scansRefused", m_refusedScans);
    if (!m_lastRefusal.empty()) {
        body.string("lastRefusal", m_lastRefusal);
    }
    body.string("note",
                "the hit list is capped at " + std::to_string(kReported) +
                    " and ordered by how much the value moved, so a longer list is not printed in "
                    "full and the first entries are the ones that moved most");
    JsonBody hits;
    for (size_t index = 0; index < m_hits.size() && index < kReported; index++) {
        const Hit& hit = m_hits[index];
        JsonBody one;
        one.string("address", hexValue(hit.address));
        one.number("offsetInRange", hit.offset);
        one.number("biggestDelta", static_cast<uint64_t>(hit.biggestDelta * 1e6));
        one.number("rowsOffUnitBy", static_cast<uint64_t>(hit.scale * 1e6));
        one.string("class", hit.rigid ? "rigid" : "scaled");
        hits.object(std::to_string(index), one.text());
    }
    body.object("hits", hits.text());
    return body.text();
}

} // namespace wiiuport::title
