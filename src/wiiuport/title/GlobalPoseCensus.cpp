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

std::string number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.6g", static_cast<double>(value));
    return {text};
}

std::string hexValue(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return {text};
}

} // namespace

size_t GlobalPoseCensus::scan(std::string& refusal) {
    refusal.clear();
    const uint64_t firstFrame = m_frame ? m_frame() : 0;

    // Snapshot one, then wait for the frame counter to move before snapshot two. **A frame apart is
    // the whole point**: two readings taken back to back would report a transform that has not
    // moved as one that has, and that mistake is the whole difference between finding the camera
    // and finding every basis matrix in the data area.
    std::vector<uint32_t> before(kWords);
    std::vector<uint32_t> after(kWords);
    if (!m_readWords(kStart, before.data(), static_cast<uint32_t>(kWords))) {
        refusal = "the guest would not give up " + std::to_string(kWords) + " words at " +
                  hexValue(kStart) + ", so the scan read nothing";
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
    if (!m_readWords(kStart, after.data(), static_cast<uint32_t>(kWords))) {
        refusal = "the guest would not give up the second reading at " + hexValue(kStart);
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
    for (size_t word = 0; word + TransformShape::kWords <= kWords; word++) {
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
        hit.address = kStart + hit.offset;
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
    const uint32_t span = TransformShape::kWords * 4;
    std::vector<Hit> kept;
    for (const Hit& hit : m_hits) {
        const bool nearAKeptOne =
            std::any_of(kept.begin(), kept.end(), [span, &hit](const Hit& other) {
                const uint32_t gap = hit.address > other.address ? hit.address - other.address
                                                                 : other.address - hit.address;
                return gap < span;
            });
        if (!nearAKeptOne) {
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
    body.string("range", hexValue(kStart) + "-" + hexValue(kEnd));
    body.number("rangeBytes", kEnd - kStart);
    body.number("windowsTested", kWords - TransformShape::kWords + 1);
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
