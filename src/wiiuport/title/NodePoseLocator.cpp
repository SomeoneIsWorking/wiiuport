#include "wiiuport/title/NodePoseLocator.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace wiiuport::title {

namespace {

// Nine significant digits, not six fixed decimals: a difference of one part in 10^40 prints
// as zero under %.6f, and a reader told "moved 18, biggest delta 0.000000" has no way to
// tell a bit of noise from a value that was never computed.
std::string number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(value));
    return {text};
}

std::string hexValue(uint32_t value) {
    std::array<char, 11> text{};
    std::snprintf(text.data(), text.size(), "0x%08x", value);
    return {text.data()};
}

} // namespace

NodePoseLocator::NodePoseLocator(Register registerProbe, ReadWords readWords)
    : m_register(registerProbe), m_readWords(readWords) {
}

const char* NodePoseLocator::nameOf(Kind kind) {
    switch (kind) {
    case Kind::Node:
        return "node";
    case Kind::SubObject:
        return "subObject";
    case Kind::Count:
        break;
    }
    return "unknown";
}

void NodePoseLocator::install() {
    // The entry's own word, read before the probe is asked for it. The fork refuses the
    // install when the entry does not hold the word the probe names, and reports that as
    // `entryHeldOther` -- true, and silent about what was there instead.
    std::array<uint32_t, 1> word{};
    if (m_readWords(kDraw, word.data(), 1)) {
        m_entryWord = word[0];
    }
    m_register(kDraw, kFirstInstruction, m_draw, true, 0);
}

void NodePoseLocator::Draw::OnInstall(GuestCallProbes::Installation installation) {
    std::scoped_lock lock(m_owner.m_mutex);
    m_owner.m_installation = installation;
}

void NodePoseLocator::Draw::OnCall(std::span<const uint32_t, 32> gpr, uint32_t /*returnAddress*/) {
    m_owner.m_calls.fetch_add(1, std::memory_order_relaxed);
    m_owner.observe(gpr[kNodeRegister], Kind::Node);
}

void NodePoseLocator::observe(uint32_t address, Kind kind) {
    scan(address, kind);
}

bool NodePoseLocator::isPose(const float* words) {
    for (size_t row = 0; row < 3; row++) {
        const float x = words[row * 3 + 0];
        const float y = words[row * 3 + 1];
        const float z = words[row * 3 + 2];
        if (std::fabs(std::sqrt(x * x + y * y + z * z) - 1.0f) > kUnitTolerance) {
            return false;
        }
    }
    for (size_t first = 0; first < 3; first++) {
        for (size_t second = first + 1; second < 3; second++) {
            float dot = 0.0f;
            for (size_t column = 0; column < 3; column++) {
                dot += words[first * 3 + column] * words[second * 3 + column];
            }
            if (std::fabs(dot) > kPerpendicularTolerance) {
                return false;
            }
        }
    }
    return true;
}

bool NodePoseLocator::claimLocked(uint32_t address, Kind kind) {
    auto known =
        std::find_if(m_tracked.begin(), m_tracked.end(), [address, kind](const Tracked& one) {
            return one.address == address && one.kind == kind;
        });
    if (known != m_tracked.end()) {
        if (known->scans >= kScansPerObject) {
            return false;
        }
        known->scans++;
        return true;
    }
    const size_t which = static_cast<size_t>(kind);
    if (trackedOfKindLocked(kind) >= kObjects) {
        m_refusedOfKind[which]++;
        return false;
    }
    m_tracked.push_back(Tracked{address, kind, 1, {}});
    return true;
}

uint32_t NodePoseLocator::windowWords(Kind kind) {
    return kind == Kind::Node ? kNodeScanWords : kScanWords;
}

void NodePoseLocator::scan(uint32_t address, Kind kind) {
    if (address == 0) {
        return;
    }
    {
        std::scoped_lock lock(m_mutex);
        if (!claimLocked(address, kind)) {
            return;
        }
    }

    const uint32_t window = windowWords(kind);
    std::vector<float> words(window, 0.0f);
    if (!m_readWords(address, reinterpret_cast<uint32_t*>(words.data()), window)) {
        std::scoped_lock lock(m_mutex);
        m_unreadable++;
        return;
    }

    // Every 4-aligned offset, tested. A float array, so the scan walks in floats and the
    // report names bytes.
    std::vector<std::pair<uint32_t, size_t>> found;
    for (size_t offset = 0; offset + kPoseWords <= words.size(); offset++) {
        if (isPose(words.data() + offset)) {
            found.emplace_back(static_cast<uint32_t>(offset * sizeof(float)), offset);
        }
    }
    if (found.empty()) {
        return;
    }

    std::scoped_lock lock(m_mutex);
    auto& tracked =
        *std::find_if(m_tracked.begin(), m_tracked.end(), [address, kind](const Tracked& one) {
            return one.address == address && one.kind == kind;
        });
    for (const auto& [byteOffset, at] : found) {
        auto known = std::find_if(tracked.candidates.begin(), tracked.candidates.end(),
                                  [byteOffset](const Candidate& one) {
                                      return one.offset == byteOffset;
                                  });
        if (known == tracked.candidates.end()) {
            tracked.candidates.push_back(Candidate{});
            known = tracked.candidates.end() - 1;
            known->offset = byteOffset;
        }
        if (known->held) {
            known->compared++;
            float biggest = 0.0f;
            for (size_t word = 0; word < kPoseWords; word++) {
                biggest = std::max(biggest, std::fabs(words[at + word] - known->last[word]));
            }
            if (biggest > kMotionEpsilon) {
                known->moved++;
            } else {
                known->still++;
            }
            known->biggestDelta = std::max(known->biggestDelta, biggest);
        }
        known->held = true;
        known->scans++;
        for (size_t word = 0; word < kPoseWords; word++) {
            known->last[word] = words[at + word];
        }
    }
}

uint32_t NodePoseLocator::trackedOfKindLocked(Kind kind) const {
    uint32_t count = 0;
    for (const Tracked& one : m_tracked) {
        if (one.kind == kind) {
            count++;
        }
    }
    return count;
}

uint32_t NodePoseLocator::neededLocked(Kind kind) const {
    const uint32_t tracked = trackedOfKindLocked(kind);
    if (tracked < 2) {
        return 0;
    }
    return tracked / 2 + 1;
}

uint32_t NodePoseLocator::bestOffset(Kind kind) const {
    std::scoped_lock lock(m_mutex);
    return bestOffsetLocked(kind);
}

uint32_t NodePoseLocator::bestOffsetLocked(Kind kind) const {
    const uint32_t needed = neededLocked(kind);
    if (needed == 0) {
        return 0;
    }
    // Offset -> how many distinct objects of this kind held a transform there.
    std::map<uint32_t, uint32_t> across;
    for (const Tracked& one : m_tracked) {
        if (one.kind != kind) {
            continue;
        }
        for (const Candidate& candidate : one.candidates) {
            if (candidate.scans > 0) {
                across[candidate.offset]++;
            }
        }
    }
    // Offset -> how many of the objects that hold it have also seen it move.
    std::map<uint32_t, uint32_t> moving;
    for (const Tracked& one : m_tracked) {
        if (one.kind != kind) {
            continue;
        }
        for (const Candidate& candidate : one.candidates) {
            if (candidate.moved > 0) {
                moving[candidate.offset]++;
            }
        }
    }
    uint32_t best = 0;
    uint32_t bestObjects = 0;
    for (const auto& [offset, count] : across) {
        if (count < needed) {
            continue;
        }
        // And it has to move somewhere. A rigid triple at the same offset in every object
        // that never changes is a basis or a normal, not a pose, and the first run of this
        // named one at three offsets 1020 bytes apart on a bitwise difference of 10^-40.
        if (moving[offset] == 0) {
            continue;
        }
        if (count > bestObjects) {
            bestObjects = count;
            best = offset;
        }
    }
    return best;
}

std::string NodePoseLocator::reportFor(Kind kind) const {
    const uint32_t tracked = trackedOfKindLocked(kind);
    const uint32_t needed = neededLocked(kind);
    JsonBody body;
    body.string("what", nameOf(kind));
    body.number("objectsTracked", tracked);
    body.number("objectsRefused", m_refusedOfKind[static_cast<size_t>(kind)]);
    body.number("scanWords", windowWords(kind));
    body.number("scanBytes", windowWords(kind) * 4);
    body.number("objectsNeeded", needed);
    const uint32_t best = bestOffsetLocked(kind);
    body.raw("bestOffset", best == 0 ? "null" : std::to_string(best));

    // The cross-object count, which is the locator's whole answer: an offset in every object
    // is a field, an offset in one is a coincidence.
    std::map<uint32_t, uint32_t> across;
    for (const Tracked& one : m_tracked) {
        if (one.kind != kind) {
            continue;
        }
        for (const Candidate& candidate : one.candidates) {
            if (candidate.scans > 0) {
                across[candidate.offset]++;
            }
        }
    }
    JsonBody offsets;
    size_t shown = 0;
    for (const auto& [offset, count] : across) {
        if (shown >= kObjects) {
            break;
        }
        uint32_t scans = 0;
        uint32_t compared = 0;
        uint32_t moved = 0;
        uint32_t still = 0;
        float biggest = 0.0f;
        for (const Tracked& one : m_tracked) {
            if (one.kind != kind) {
                continue;
            }
            for (const Candidate& candidate : one.candidates) {
                if (candidate.offset != offset) {
                    continue;
                }
                scans += candidate.scans;
                compared += candidate.compared;
                moved += candidate.moved;
                still += candidate.still;
                biggest = std::max(biggest, candidate.biggestDelta);
            }
        }
        JsonBody one;
        one.number("offset", offset);
        one.number("objects", count);
        one.number("scans", scans);
        one.number("compared", compared);
        one.number("moved", moved);
        one.number("still", still);
        one.raw("biggestDelta", number(biggest));
        one.raw("moving", moved > 0 ? "true" : "false");
        offsets.object(std::to_string(shown), one.text());
        shown++;
    }
    body.object("offsets", offsets.text());
    return body.finish();
}

std::string NodePoseLocator::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("draw", hexValue(kDraw));
    body.string("firstInstruction", hexValue(kFirstInstruction));
    // What the entry actually held, and whether the two agree. A mismatch is why the fork
    // refused the install, so it is the first thing a reader of a refusal needs.
    body.string("entryWordAtInstall", hexValue(m_entryWord));
    body.raw("entryWordMatches", m_entryWord == kFirstInstruction ? "true" : "false");
    body.string("vtableTargetNotProbed", hexValue(kVtableTarget));
    body.string("probe", std::string([this] {
                    if (!m_installation.has_value()) {
                        return "pending";
                    }
                    switch (*m_installation) {
                    case GuestCallProbes::Installation::Installed:
                        return "installed";
                    case GuestCallProbes::Installation::EntryHeldOther:
                        return "entryHeldOther";
                    case GuestCallProbes::Installation::EntryNotRelocatable:
                        return "entryNotRelocatable";
                    case GuestCallProbes::Installation::NoCodeSpace:
                        return "noCodeSpace";
                    }
                    return "unknown";
                }()));
    body.number("nodeRegister", kNodeRegister);
    body.number("subObjectOffset", kSubObjectOffset);
    body.number("calls", m_calls.load());
    body.raw("motionEpsilon", number(kMotionEpsilon));
    body.number("subObjectScanWords", kScanWords);
    body.number("nodeScanWords", kNodeScanWords);
    body.number("nodeScanBytes", kNodeScanWords * 4);
    body.number("subObjectAtNodeOffset", kSubObjectOffset);
    body.number("scansPerObject", kScansPerObject);
    body.number("readsUnreadable", m_unreadable);
    // The two kinds, scored apart. A field at the same offset in a node and in its
    // sub-object would be one coincidence seen twice if they shared a table, so they do not.
    body.object("node", reportFor(Kind::Node));
    body.object("subObject", reportFor(Kind::SubObject));
    return body.finish();
}

} // namespace wiiuport::title
