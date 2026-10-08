#include "wiiuport/title/ObjectPoseHistory.h"

#include "wiiuport/title/JsonBody.h"
#include "wiiuport/title/TransformShape.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

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
    std::snprintf(text, sizeof(text), "%.6f", static_cast<double>(value));
    return {text};
}

std::string hexValue(uint32_t value) {
    std::array<char, 11> text{};
    std::snprintf(text.data(), text.size(), "0x%08x", value);
    return {text.data()};
}

// Twelve words as the four groups of three the title's own dump showed, printed as the
// numbers they are: a tool that reformats them cannot be compared with that evidence by
// eye.
std::string poseJson(const std::array<uint32_t, ObjectPoseHistory::kPoseWords>& words) {
    JsonBody body;
    for (size_t row = 0; row < ObjectPoseHistory::kPoseWords / 3; row++) {
        JsonBody line;
        for (size_t column = 0; column < 3; column++) {
            line.raw(std::to_string(column),
                     JsonBody::real(static_cast<double>(asFloat(words[row * 3 + column])), 6));
        }
        body.object(std::to_string(row), line.text());
    }
    return body.text();
}

} // namespace

ObjectPoseHistory::ObjectPoseHistory(ReadWords readWords) : m_readWords(readWords) {
}

// A rigid transform, tested.
//
// Three rows of unit length and mutually perpendicular is what makes twelve floats a
// pose rather than three rows of numbers that happen to be near unit length, and it is
// the same test the title's own evidence was put through. A block that fails is not an
// error: it is a block with something else at `+0xc4`, and the count of those is the
// report's answer to which blocks a blend can act on.
bool ObjectPoseHistory::isPose(const std::array<uint32_t, kPoseWords>& words) {
    auto at = [&words](size_t index) {
        return asFloat(words[index]);
    };
    for (size_t row = 0; row < 3; row++) {
        const float x = at(row * 3 + 0);
        const float y = at(row * 3 + 1);
        const float z = at(row * 3 + 2);
        const float length = std::sqrt(x * x + y * y + z * z);
        if (std::fabs(length - 1.0f) > kUnitTolerance) {
            return false;
        }
    }
    for (size_t first = 0; first < 3; first++) {
        for (size_t second = first + 1; second < 3; second++) {
            float dot = 0.0f;
            for (size_t column = 0; column < 3; column++) {
                dot += at(first * 3 + column) * at(second * 3 + column);
            }
            if (std::fabs(dot) > kPerpendicularTolerance) {
                return false;
            }
        }
    }
    return true;
}

bool ObjectPoseHistory::readPose(uint32_t block, std::array<uint32_t, kPoseWords>& pose) const {
    if (block == 0) {
        return false;
    }
    return m_readWords(block + kPoseOffset, pose.data(), kPoseWords);
}

void ObjectPoseHistory::observe(uint32_t object, uint32_t block, uint32_t otherBlock) {
    m_observations.fetch_add(1, std::memory_order_relaxed);
    if (object == 0) {
        std::scoped_lock lock(m_mutex);
        m_noObject++;
        return;
    }
    std::array<uint32_t, kPoseWords> pose{};
    // The whole block, for the offset scan, for the first few blocks only. Read before
    // the lock like the pose: a binding on the display thread must not queue behind a
    // report being written.
    std::array<uint32_t, kBlockWords> whole{};
    bool whole_ = false;
    {
        // The scan list is shared, so choosing which blocks to scan is under the lock.
        // A block is scanned at *every* one of its bindings, not once: the block may
        // hold the pose only after the frame's draw has filled it, and a single scan at
        // one moment would find a block that holds no pose and call it a wrong offset.
        std::scoped_lock scanLock(m_mutex);
        if (block != 0) {
            auto known = std::find(m_scanned.begin(), m_scanned.end(), block);
            if (known == m_scanned.end()) {
                if (m_scanned.size() >= kScanned) {
                    known = m_scanned.end();
                } else {
                    m_scanned.push_back(block);
                    known = m_scanned.end() - 1;
                }
            }
            whole_ = known != m_scanned.end() && m_readWords(block, whole.data(), kBlockWords);
        }
    }
    const bool readable = readPose(block, pose);
    const bool carries = readable && isPose(pose);

    std::scoped_lock lock(m_mutex);
    if (whole_) {
        m_scans++;
        // Every 4-aligned offset with a rigid transform's twelve words in it. A
        // transform is three rows of three with a fourth group beside it, so the last
        // candidate offset is two words short of the block's end.
        for (size_t offset = 0; offset + kPoseWords <= kBlockWords; offset++) {
            std::array<uint32_t, kPoseWords> candidate{};
            for (size_t word = 0; word < kPoseWords; word++) {
                candidate[word] = whole[offset + word];
            }
            if (isPose(candidate)) {
                m_offsets[offset]++;
            }
        }
        // The same windows, asked of `TransformShape::isAffine` -- the class that also
        // accepts a scaled pose. Reported beside the rigid one rather than instead of it: a
        // rigid hit is a pose with no scale, an affine hit is a pose with scale, and which of
        // the two the block holds is the difference between "the pose is at this offset" and
        // "the pose is nowhere here". The translation is the last group of the twelve, which
        // is where `TransformShape` reads it.
        for (size_t offset = 0; offset + kPoseWords <= kBlockWords; offset++) {
            if (!Shape::isAffine(reinterpret_cast<const float*>(whole.data()) + offset)) {
                continue;
            }
            m_affineHits++;
            m_affineOffsets[offset]++;
            if (m_affineExampleOffset == 0 && offset != 0) {
                // One example, as numbers, so a reader can see what matched rather than
                // taking the count's word for it. The block is gone by the time a report is
                // asked for, so it is captured here. Offset zero is skipped for the example
                // only -- a matrix at the block's first word is still counted.
                m_affineExampleOffset = static_cast<uint64_t>(offset * 4);
                m_affineExample.clear();
                for (size_t word = 0; word < kPoseWords; word++) {
                    m_affineExample +=
                        (word == 0 ? "" : " ") + number(asFloat(whole[offset + word]));
                }
                for (size_t axis = 0; axis < 3; axis++) {
                    m_affineTranslation[axis] = asFloat(whole[offset + 9 + axis]);
                }
            }
        }
    }
    if (!readable) {
        m_unreadable++;
        return;
    }
    if (!carries) {
        m_notPoseBlocks++;
        return;
    }
    m_poseBlocks++;

    // The other slot, when it is a pose too: that is the ring carrying the previous
    // tick, and it is only a question when there is something in it.
    std::array<uint32_t, kPoseWords> other{};
    const bool otherCarries =
        otherBlock != 0 && otherBlock != block && readPose(otherBlock, other) && isPose(other);

    auto known = std::find_if(m_series.begin(), m_series.end(), [block](const Series& entry) {
        return entry.block == block;
    });
    if (known == m_series.end()) {
        if (m_series.size() >= kBlocks) {
            m_refused++;
            return;
        }
        m_series.emplace_back();
        known = m_series.end() - 1;
        known->block = block;
    }
    Series& entry = *known;
    entry.object = object;
    entry.otherBlock = otherBlock;
    if (entry.held > 0) {
        const std::array<uint32_t, kPoseWords>& previous = entry.history[0];
        entry.compared++;
        size_t differing = 0;
        float biggest = 0.0f;
        for (size_t word = 0; word < kPoseWords; word++) {
            if (previous[word] == pose[word]) {
                continue;
            }
            differing++;
            // The words differ; how far apart they are as floats is what separates an
            // object that moved from a mantissa bit that did. Compared as floats,
            // because the distance between two floats is not the distance between their
            // words: a sign bit makes two near-equal words differ by billions.
            biggest = std::max(biggest, std::fabs(asFloat(pose[word]) - asFloat(previous[word])));
        }
        if (differing > 0) {
            entry.changed++;
            entry.changedWords += differing;
            entry.biggestDelta = std::max(entry.biggestDelta, biggest);
        }
        if (otherCarries) {
            entry.seenAsOther++;
            if (other == previous) {
                entry.otherMatched++;
            }
        }
    }
    // Keep the reading, shifting the ring. The bound is the ring's own size, so a full
    // ring drops its oldest reading rather than writing past its end -- which is what
    // leaving the shift out did, and the second reading came back as zeroes.
    for (size_t index = std::min(entry.held + 1, kHistory - 1); index > 0; index--) {
        entry.history[index] = entry.history[index - 1];
    }
    entry.history[0] = pose;
    if (entry.held < kHistory) {
        entry.held++;
    }
}

std::string ObjectPoseHistory::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("poseOffset", hexValue(kPoseOffset));
    body.number("poseWords", kPoseWords);
    body.number("unitTolerance", static_cast<uint64_t>(kUnitTolerance * 1000.0f));
    body.number("perpendicularTolerance", static_cast<uint64_t>(kPerpendicularTolerance * 1000.0f));
    // The second class, and the denominator it was measured over: how many windows it was
    // asked about, so a zero here means "no scaled pose in any of these windows" rather than
    // "not looked for". The rigid scan's own counts are the `m_offsets` table below.
    body.number("affineWindowsScanned", kBlockWords - kPoseWords + 1);
    body.number("affineHits", m_affineHits);
    {
        uint64_t offsetHits = 0;
        std::string where;
        for (size_t offset = 0; offset < m_affineOffsets.size(); offset++) {
            if (m_affineOffsets[offset] == 0) {
                continue;
            }
            offsetHits += m_affineOffsets[offset];
            where += (where.empty() ? "" : " ") + std::to_string(offset * 4) +
                     "b:" + std::to_string(m_affineOffsets[offset]);
        }
        body.number("affineOffsetsWithHits", offsetHits);
        body.string("affineOffsetList", where);
    }
    if (!m_affineExample.empty()) {
        body.string("affineExample", m_affineExample);
        body.number("affineExampleOffsetBytes", m_affineExampleOffset);
        for (size_t axis = 0; axis < 3; axis++) {
            body.signedNumber("affineExampleTranslation" + std::to_string(axis),
                              static_cast<int64_t>(m_affineTranslation[axis] * 1000.0f));
        }
    }
    body.number("observations", m_observations.load());
    body.number("poseBindings", m_poseBlocks);
    body.number("notPoseBindings", m_notPoseBlocks);
    body.number("bindingsUnreadable", m_unreadable);
    body.number("bindingsWithoutObject", m_noObject);
    body.number("blocksRefused", m_refused);

    // The offset scan, and the one number that says whether it found anything.
    body.number("scans", m_scans);
    body.number("scannedBlocks", m_scanned.size());
    {
        uint64_t hits = 0;
        std::string where;
        for (size_t offset = 0; offset < m_offsets.size(); offset++) {
            if (m_offsets[offset] == 0) {
                continue;
            }
            hits += m_offsets[offset];
            char text[24];
            std::snprintf(text, sizeof(text), " 0x%02zx:%llu", offset * 4,
                          static_cast<unsigned long long>(m_offsets[offset]));
            where += text;
        }
        body.number("offsetHits", hits);
        if (hits == 0) {
            body.raw("poseOffsets", "null");
        } else {
            body.string("poseOffsets", where);
        }
    }

    // The one number that decides where a blend reads from, over the count that says
    // how many chances it had. Null when nothing was compared, because "no comparisons"
    // is not a measurement of "not written before the bind".
    uint64_t compared = 0;
    uint64_t changed = 0;
    for (const Series& entry : m_series) {
        compared += entry.compared;
        changed += entry.changed;
    }
    if (compared == 0) {
        body.raw("writtenBeforeBind", "null");
    } else {
        body.raw("writtenBeforeBind", changed > 0 ? "true" : "false");
    }
    body.number("compared", compared);
    body.number("changed", changed);

    JsonBody blocks;
    for (size_t index = 0; index < m_series.size() && index < kExamples; index++) {
        const Series& entry = m_series[index];
        JsonBody one;
        one.string("block", hexValue(entry.block));
        one.string("object", hexValue(entry.object));
        one.string("otherBlock", hexValue(entry.otherBlock));
        one.number("readings", entry.held);
        one.number("compared", entry.compared);
        one.number("changed", entry.changed);
        one.number("changedWords", entry.changedWords);
        one.raw("biggestDelta", JsonBody::real(static_cast<double>(entry.biggestDelta), 6));
        one.number("seenAsOther", entry.seenAsOther);
        one.number("otherMatched", entry.otherMatched);
        one.raw("pose", poseJson(entry.history[0]));
        if (entry.held > 1) {
            one.raw("previousPose", poseJson(entry.history[1]));
        }
        blocks.object(std::to_string(index), one.text());
    }
    body.object("blocks", blocks.text());
    return body.finish();
}

} // namespace wiiuport::title
