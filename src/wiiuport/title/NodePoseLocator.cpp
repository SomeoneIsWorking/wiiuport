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

} // namespace

NodePoseLocator::NodePoseLocator(Register registerProbe, ReadWords readWords)
    : m_register(registerProbe), m_readWords(readWords) {
}

void NodePoseLocator::install() {
    m_register(kDraw, kFirstInstruction, m_draw, true, 0);
}

void NodePoseLocator::Draw::OnInstall(GuestCallProbes::Installation installation) {
    std::scoped_lock lock(m_owner.m_mutex);
    m_owner.m_installation = installation;
}

void NodePoseLocator::Draw::OnCall(std::span<const uint32_t, 32> gpr, uint32_t /*returnAddress*/) {
    m_owner.m_calls.fetch_add(1, std::memory_order_relaxed);
    m_owner.scan(gpr[kNodeRegister]);
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

void NodePoseLocator::scan(uint32_t node) {
    if (node == 0) {
        return;
    }
    // Whether this node is tracked, and whether it has scans left, is a shared decision, so
    // it is made under the lock; the read of the node's memory is not, so a scan does not
    // hold the display thread's lock while it reads.
    {
        std::scoped_lock lock(m_mutex);
        auto known = std::find_if(m_nodes.begin(), m_nodes.end(), [node](const Node& one) {
            return one.address == node;
        });
        if (known == m_nodes.end()) {
            if (m_nodes.size() >= kNodes) {
                m_refused++;
                return;
            }
            m_nodes.emplace_back();
            known = m_nodes.end() - 1;
            known->address = node;
        }
        if (known->scans >= kScansPerNode) {
            return;
        }
        known->scans++;
    }

    std::vector<float> words(kScanWords, 0.0f);
    if (!m_readWords(node, reinterpret_cast<uint32_t*>(words.data()), kScanWords)) {
        std::scoped_lock lock(m_mutex);
        m_unreadable++;
        return;
    }

    // Every 4-aligned offset, tested. A float array, so an offset in floats is what the
    // scan walks in and the report names in bytes.
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
    auto& tracked = *std::find_if(m_nodes.begin(), m_nodes.end(), [node](const Node& one) {
        return one.address == node;
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
            bool differing = false;
            for (size_t word = 0; word < kPoseWords; word++) {
                biggest = std::max(biggest, std::fabs(words[at + word] - known->last[word]));
                if (words[at + word] != known->last[word]) {
                    differing = true;
                }
            }
            if (differing) {
                known->moved++;
                known->biggestDelta = std::max(known->biggestDelta, biggest);
            }
        }
        known->held = true;
        known->scans++;
        for (size_t word = 0; word < kPoseWords; word++) {
            known->last[word] = words[at + word];
        }
    }
}

uint32_t NodePoseLocator::bestOffset() const {
    std::scoped_lock lock(m_mutex);
    return bestOffsetLocked();
}

uint32_t NodePoseLocator::bestOffsetLocked() const {
    if (m_nodes.size() < 2) {
        // One node cannot agree with another, and the cross-node test is the whole of the
        // locator. A single node's offsets are reported; none is named.
        return 0;
    }
    // Offset -> how many distinct nodes held a transform there.
    std::map<uint32_t, uint32_t> nodes;
    for (const Node& node : m_nodes) {
        for (const Candidate& candidate : node.candidates) {
            if (candidate.scans > 0) {
                nodes[candidate.offset]++;
            }
        }
    }
    const uint32_t needed = static_cast<uint32_t>(m_nodes.size()) / 2 + 1;
    uint32_t best = 0;
    uint32_t bestNodes = 0;
    for (const auto& [offset, count] : nodes) {
        if (count >= needed && count > bestNodes) {
            bestNodes = count;
            best = offset;
        }
    }
    return best;
}

std::string NodePoseLocator::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("draw", hexValue(kDraw));
    body.string("firstInstruction", hexValue(kFirstInstruction));
    body.number("nodeRegister", kNodeRegister);
    // The vtable's target, reported because it is *not* the probed address and a reader
    // comparing the two should not have to find that out from the code.
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
    body.number("calls", m_calls.load());
    body.number("nodesTracked", m_nodes.size());
    body.number("nodesRefused", m_refused);
    body.number("readsUnreadable", m_unreadable);
    body.number("scanWords", kScanWords);
    body.number("scansPerNode", kScansPerNode);

    // The cross-node count, which is the locator's whole answer: an offset in every node is
    // a field, an offset in one is a coincidence.
    std::map<uint32_t, uint32_t> across;
    for (const Node& node : m_nodes) {
        for (const Candidate& candidate : node.candidates) {
            if (candidate.scans > 0) {
                across[candidate.offset]++;
            }
        }
    }
    JsonBody offsets;
    size_t shown = 0;
    for (const auto& [offset, count] : across) {
        if (shown >= kNodes) {
            break;
        }
        uint32_t scans = 0;
        uint32_t compared = 0;
        uint32_t moved = 0;
        float biggest = 0.0f;
        for (const Node& node : m_nodes) {
            for (const Candidate& candidate : node.candidates) {
                if (candidate.offset != offset) {
                    continue;
                }
                scans += candidate.scans;
                compared += candidate.compared;
                moved += candidate.moved;
                biggest = std::max(biggest, candidate.biggestDelta);
            }
        }
        JsonBody one;
        one.number("offset", offset);
        one.number("nodes", count);
        one.number("scans", scans);
        one.number("compared", compared);
        one.number("moved", moved);
        one.raw("biggestDelta", number(biggest));
        offsets.object(std::to_string(shown), one.text());
        shown++;
    }
    body.object("offsets", offsets.text());
    const uint32_t needed = m_nodes.size() < 2 ? 0 : static_cast<uint32_t>(m_nodes.size()) / 2 + 1;
    body.number("nodesNeeded", needed);
    const uint32_t best = bestOffsetLocked();
    body.raw("bestOffset", best == 0 ? "null" : std::to_string(best));
    return body.finish();
}

} // namespace wiiuport::title
