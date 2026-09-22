#include "wiiuport/interp/ObjectPlanner.h"

#include "wiiuport/interp/Blendable.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace wiiuport::interp {

namespace {

// How far an object moved over two frames, and how far its midpoint lands
// from `between`, over the values that are numbers in all three.
struct Distances {
    double step{0.0};
    double residual{0.0};
};

Distances measure(std::span<const float> before, std::span<const float> after,
                  std::span<const float> between) {
    Distances squared;
    for (size_t index = 0; index < after.size(); ++index) {
        float a = before[index];
        float b = after[index];
        float c = between[index];
        if (!isNumber(a) || !isNumber(b) || !isNumber(c)) {
            continue;
        }
        double moved = static_cast<double>(b) - a;
        double off = ((static_cast<double>(a) + b) / 2.0) - c;
        squared.step += moved * moved;
        squared.residual += off * off;
    }
    return {std::sqrt(squared.step), std::sqrt(squared.residual)};
}

bool landsOn(const Distances& distances) {
    return distances.residual <= ObjectPlanner::kPartnerTolerance * distances.step;
}

bool equalValues(std::span<const float> l, std::span<const float> r) {
    return l.size() == r.size() && std::memcmp(l.data(), r.data(), l.size_bytes()) == 0;
}

} // namespace

std::string_view ObjectPlanner::outcomeName(Outcome outcome) {
    switch (outcome) {
    case Outcome::Blended:
        return "blended";
    case Outcome::Held:
        return "held";
    case Outcome::Unmatched:
        return "unmatched";
    case Outcome::Unverified:
        return "unverified";
    case Outcome::Count:
        break;
    }
    return "unknown";
}

ObjectPlanner::ObjectPlanner(float t) : m_s((1.0f + t) / 2.0f) {
}

void ObjectPlanner::Plan::clear() {
    blendedAt.clear();
    floats.clear();
    outcomes = {};
}

void ObjectPlanner::add(const frame::RecordedUniformAssembly& assembly) {
    size_t entry = m_building.add(assembly);
    m_buildingPlan.blendedAt.push_back(kNotBlended);
    // Planned only against two whole frames; before that the frame is kept
    // as history and nothing more.
    if (m_framesHeld >= 2) {
        ++m_buildingPlan.outcomes[static_cast<size_t>(plan(entry))];
    }
}

void ObjectPlanner::endFrame() {
    m_building.finish();
    ++m_framesPlanned;
    // Swapping keeps every frame's storage, so a steady scene allocates
    // nothing frame to frame.
    std::swap(m_frames[2], m_frames[1]);
    std::swap(m_frames[1], m_frames[0]);
    std::swap(m_frames[0], m_building);
    std::swap(m_plan, m_buildingPlan);
    m_building.begin();
    m_buildingPlan.clear();
    m_framesHeld = std::min(m_framesHeld + 1, m_frames.size());
    // Expired entries go once the table outgrows a frame's worth, so objects
    // that left the scene do not accumulate.
    if (m_searchAgainAt.size() > m_frames[0].size()) {
        std::erase_if(m_searchAgainAt, [this](const auto& failed) {
            return failed.second <= m_framesPlanned;
        });
    }
}

void ObjectPlanner::forget() {
    m_framesHeld = 0;
    m_building.begin();
    m_buildingPlan.clear();
}

std::optional<AssemblyKey> ObjectPlanner::derivedPartner(const AssemblyKey& key) const {
    AssemblyKey partner = key;
    for (size_t index = 1; index < partner.sourceCount; index += 2) {
        uint32_t address = partner.sources[index];
        if (auto paired = m_blockPartner.find(address); paired != m_blockPartner.end()) {
            partner.sources[index] = paired->second;
            continue;
        }
        if (!m_frames[0].sourced(address)) {
            return std::nullopt;
        }
    }
    return partner;
}

void ObjectPlanner::learn(const AssemblyKey& key, const AssemblyKey& partner) {
    if (key.sourceCount != partner.sourceCount) {
        return;
    }
    if (m_blockPartner.size() > kMaxBlockPairs) {
        m_blockPartner.clear();
    }
    for (size_t index = 1; index < key.sourceCount; index += 2) {
        // Both ways round: next frame's blocks are this frame's partners.
        m_blockPartner.insert_or_assign(key.sources[index], partner.sources[index]);
        m_blockPartner.insert_or_assign(partner.sources[index], key.sources[index]);
    }
}

std::optional<size_t> ObjectPlanner::searchPartner(const AssemblyKey& key,
                                                   std::span<const float> before,
                                                   std::span<const float> after) {
    const KeyedFrame& between = m_frames[0];
    // The values it moved in first, then the ones it held: a different object
    // is usually told apart by the first few.
    m_searchOrder.clear();
    double stepSquared = 0.0;
    for (uint32_t index = 0; index < after.size(); ++index) {
        if (!isNumber(before[index]) || !isNumber(after[index])) {
            continue;
        }
        double moved = static_cast<double>(after[index]) - before[index];
        stepSquared += moved * moved;
        m_searchOrder.push_back(index);
    }
    std::stable_partition(m_searchOrder.begin(), m_searchOrder.end(), [&](uint32_t index) {
        return before[index] != after[index];
    });
    std::optional<size_t> best;
    double bestResidual = static_cast<double>(kPartnerTolerance) * kPartnerTolerance * stepSquared;
    auto consider = [&](uint32_t entry) {
        std::span<const float> candidate = between.values(entry);
        if (candidate.size() != after.size()) {
            return;
        }
        double residual = 0.0;
        for (uint32_t index : m_searchOrder) {
            if (!isNumber(candidate[index])) {
                continue;
            }
            double off =
                ((static_cast<double>(before[index]) + after[index]) / 2.0) - candidate[index];
            residual += off * off;
            if (residual > bestResidual) {
                return;
            }
        }
        bestResidual = residual;
        best = entry;
    };
    KeyedFrame::ShaderDraws draws = between.drawnBy(key.shader);
    // A partner lands within the tolerance on every value, so on the one its
    // shader's draws are ordered by too: only those near the midpoint there
    // can be it. Exact, not a guess -- the rest would fail the sum below.
    std::span<const uint32_t> near = draws.ordered;
    uint32_t at = draws.position;
    if (at < after.size() && isNumber(before[at]) && isNumber(after[at])) {
        double centre = (static_cast<double>(before[at]) + after[at]) / 2.0;
        double radius =
            std::nextafter(std::sqrt(bestResidual), std::numeric_limits<double>::infinity());
        near = between.within(draws, centre - radius, centre + radius);
    }
    for (uint32_t entry : near) {
        consider(entry);
    }
    for (uint32_t entry : draws.unordered) {
        consider(entry);
    }
    return best;
}

bool ObjectPlanner::findPartner(const AssemblyKey& key, std::span<const float> before,
                                std::span<const float> after) {
    const KeyedFrame& between = m_frames[0];
    if (std::optional<AssemblyKey> derived = derivedPartner(key)) {
        std::optional<size_t> entry = between.find(*derived);
        if (entry.has_value() && between.values(*entry).size() == after.size() &&
            landsOn(measure(before, after, between.values(*entry)))) {
            ++m_partnersDerived;
            return true;
        }
    }
    uint64_t hash = key.hash();
    if (auto failed = m_searchAgainAt.find(hash);
        failed != m_searchAgainAt.end() && failed->second > m_framesPlanned) {
        ++m_searchesDeferred;
        return false;
    }
    ++m_partnersSearched;
    std::optional<size_t> found = searchPartner(key, before, after);
    if (!found.has_value() || !landsOn(measure(before, after, between.values(*found)))) {
        m_searchAgainAt.insert_or_assign(hash, m_framesPlanned + kSearchRetryInterval);
        return false;
    }
    m_searchAgainAt.erase(hash);
    learn(key, between.key(*found));
    return true;
}

ObjectPlanner::Outcome ObjectPlanner::plan(size_t entry) {
    const KeyedFrame& latest = m_building;
    const KeyedFrame& twoBack = m_frames[1];
    const AssemblyKey& key = latest.key(entry);
    std::span<const float> after = latest.values(entry);
    std::optional<size_t> earlier = twoBack.find(key);
    if (!earlier.has_value() || twoBack.values(*earlier).size() != after.size()) {
        return Outcome::Unmatched;
    }
    std::span<const float> before = twoBack.values(*earlier);
    if (equalValues(before, after)) {
        return Outcome::Held;
    }
    if (!findPartner(key, before, after)) {
        return Outcome::Unverified;
    }
    std::vector<float>& floats = m_buildingPlan.floats;
    m_buildingPlan.blendedAt[entry] = static_cast<uint32_t>(floats.size());
    for (size_t index = 0; index < after.size(); ++index) {
        float a = before[index];
        float b = after[index];
        if (isNumber(a) && isNumber(b)) {
            floats.push_back(a + ((b - a) * m_s));
            continue;
        }
        if (a != b) {
            ++m_valuesNotBlended;
        }
        floats.push_back(b);
    }
    return Outcome::Blended;
}

std::span<const float> ObjectPlanner::blendOf(size_t entry) const {
    uint32_t at = m_plan.blendedAt[entry];
    if (at == kNotBlended) {
        return {};
    }
    return {m_plan.floats.data() + at, m_frames[0].values(entry).size()};
}

} // namespace wiiuport::interp
