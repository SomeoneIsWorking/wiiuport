#include "wiiuport/interp/ObjectPlanner.h"

#include "wiiuport/interp/Blendable.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace wiiuport::interp {

namespace {

// Whether an object moved in a value between N-2 and N: a number at both ends
// with other bits. Only such a value says where the object passed through; one
// the same at both ends is drawn at N whatever N-1 holds, and one N-1 holds
// otherwise is flipping with the title's double buffering -- packed data, as
// one shader's, that reads as floats up to 2^97 and would swamp any measure.
bool movedAt(std::span<const float> before, std::span<const float> after, size_t index) {
    return isNumber(before[index]) && isNumber(after[index]) &&
           std::memcmp(&before[index], &after[index], sizeof(float)) != 0;
}

// How far an object moved over two frames, and how far its midpoint lands
// from `between`, over the values it moved in that are its own -- not frame
// state -- and numbers in `between`; how many values that is, and how many
// of its own it moved in at all.
struct Distances {
    double step{0.0};
    double residual{0.0};
    size_t compared{0};
    size_t moved{0};
};

Distances measure(std::span<const float> before, std::span<const float> after,
                  std::span<const float> between, const FrameState& state) {
    Distances squared;
    for (size_t index = 0; index < after.size(); ++index) {
        float a = before[index];
        float b = after[index];
        float c = between[index];
        if (!movedAt(before, after, index) || state.at(index)) {
            continue;
        }
        ++squared.moved;
        if (!isNumber(c)) {
            continue;
        }
        double moved = static_cast<double>(b) - a;
        double off = ((static_cast<double>(a) + b) / 2.0) - c;
        squared.step += moved * moved;
        squared.residual += off * off;
        ++squared.compared;
    }
    return {std::sqrt(squared.step), std::sqrt(squared.residual), squared.compared, squared.moved};
}

// Whether the candidate is the object's partner: the object's own moved
// values -- the only values taken from the partner that are not the frame's --
// pass through it, and it has a number at one of them at least, or the check
// would pass on anything. An object that moved in no value of its own draws
// the same whichever candidate is taken, so any is.
bool landsOn(const Distances& distances) {
    if (distances.moved == 0) {
        return true;
    }
    return distances.compared > 0 &&
           distances.residual <= ObjectPlanner::kPartnerTolerance * distances.step;
}

// Whether `blended` lies strictly between the object's two neighbours:
// nearer each of them than they are to each other, over the values it moved
// in that are numbers in all three. This is what an in-between frame
// promises, so it is checked on the values drawn rather than assumed from the
// partner check; a value it did not move in is drawn at N by design.
bool liesBetween(std::span<const float> before, std::span<const float> oneBack,
                 std::span<const float> blended, std::span<const float> latest) {
    double apart = 0.0;
    double fromOneBack = 0.0;
    double fromLatest = 0.0;
    for (size_t index = 0; index < latest.size(); ++index) {
        float p = oneBack[index];
        float v = blended[index];
        float b = latest[index];
        if (!movedAt(before, latest, index) || !isNumber(p) || !isNumber(v)) {
            continue;
        }
        double pb = static_cast<double>(b) - p;
        double pv = static_cast<double>(v) - p;
        double vb = static_cast<double>(b) - v;
        apart += pb * pb;
        fromOneBack += pv * pv;
        fromLatest += vb * vb;
    }
    return fromOneBack < apart && fromLatest < apart;
}

// Whether an object's last failed search of one kind was too recent to run
// it again.
bool waiting(const std::unordered_map<uint64_t, uint64_t>& againAt, uint64_t hash,
             uint64_t framesPlanned) {
    auto failed = againAt.find(hash);
    return failed != againAt.end() && failed->second > framesPlanned;
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
    case Outcome::Outside:
        return "outside";
    case Outcome::Count:
        break;
    }
    return "unknown";
}

ObjectPlanner::ObjectPlanner(float t) : m_t(t) {
}

FrameState ObjectPlanner::frameState(const ShaderKey& shader) const {
    return {m_frames[0].sharedValues(shader), m_frames[1].sharedValues(shader)};
}

void ObjectPlanner::Plan::clear() {
    blendedAt.clear();
    partnerAt.clear();
    floats.clear();
    outcomeOf.clear();
    outcomes = {};
}

void ObjectPlanner::add(const frame::RecordedUniformAssembly& assembly) {
    indexLatest();
    // Fresh blocks are told from an object's own only against a frame held
    // two back.
    size_t entry = m_building.add(assembly, m_framesHeld >= 2 ? &m_frames[1] : nullptr);
    m_buildingPlan.blendedAt.push_back(kNotBlended);
    m_buildingPlan.partnerAt.push_back(kNotBlended);
    // Planned only against two whole frames; before that the frame is kept
    // as history, and its entries read as unmatched in a plan never ready.
    Outcome outcome = m_framesHeld >= 2 ? plan(entry) : Outcome::Unmatched;
    m_buildingPlan.outcomeOf.push_back(outcome);
    ++m_buildingPlan.outcomes[static_cast<size_t>(outcome)];
}

void ObjectPlanner::endFrame() {
    // A frame with no draws never indexed the one before it, which is about
    // to become N-1 and be searched.
    indexLatest();
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
    m_latestUnindexed = true;
    m_framesHeld = std::min(m_framesHeld + 1, m_frames.size());
    // Expired entries go once the table outgrows a frame's worth, so objects
    // that left the scene do not accumulate.
    for (auto* againAt : {&m_searchAgainAt, &m_reidentifyAgainAt}) {
        if (againAt->size() > m_frames[0].size()) {
            std::erase_if(*againAt, [this](const auto& failed) {
                return failed.second <= m_framesPlanned;
            });
        }
    }
}

void ObjectPlanner::indexLatest() {
    if (m_latestUnindexed) {
        m_frames[0].indexValues();
        m_latestUnindexed = false;
    }
}

void ObjectPlanner::forget() {
    m_framesHeld = 0;
    m_latestUnindexed = false;
    m_building.begin();
    m_buildingPlan.clear();
}

std::optional<AssemblyKey> ObjectPlanner::derivedKey(const AssemblyKey& key) const {
    AssemblyKey partner = key;
    for (size_t index = 1; index < partner.sourceCount; index += 2) {
        uint32_t address = partner.sources[index];
        if (address == AssemblyKey::kFreshBlock) {
            continue;
        }
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
        if (key.sources[index] == AssemblyKey::kFreshBlock ||
            partner.sources[index] == AssemblyKey::kFreshBlock) {
            continue;
        }
        // Both ways round: next frame's blocks are this frame's partners.
        m_blockPartner.insert_or_assign(key.sources[index], partner.sources[index]);
        m_blockPartner.insert_or_assign(partner.sources[index], key.sources[index]);
    }
}

std::optional<size_t> ObjectPlanner::searchPartner(const AssemblyKey& key,
                                                   std::span<const float> before,
                                                   std::span<const float> after) {
    // The midpoint over the values the object moved in, and the values it
    // held where it held them: those pick out the object itself when it drew
    // in N-1, and let the search skip everything standing elsewhere. Frame
    // state is the same in every candidate and is left out.
    FrameState state = frameState(key.shader);
    m_point.assign(after.size(), std::numeric_limits<double>::quiet_NaN());
    double stepSquared = 0.0;
    bool moved = false;
    bool held = false;
    for (size_t index = 0; index < after.size(); ++index) {
        if (state.at(index)) {
            continue;
        }
        if (movedAt(before, after, index)) {
            double step = static_cast<double>(after[index]) - before[index];
            stepSquared += step * step;
            m_point[index] = (static_cast<double>(before[index]) + after[index]) / 2.0;
            moved = true;
        } else if (isNumber(after[index])) {
            m_point[index] = after[index];
            held = true;
        }
    }
    // An object that moved in no value of its own draws the same with any
    // candidate: the nearest in what it held is taken, however far.
    double limit = moved ? static_cast<double>(kPartnerTolerance) * kPartnerTolerance * stepSquared
                         : std::numeric_limits<double>::infinity();
    DrawTree::Nearest found = m_frames[0].nearest(key.shader, {m_point, limit, std::nullopt});
    m_partnerCandidates += found.compared;
    if (found.entry.has_value() || !held) {
        return found.entry;
    }
    // A held value N-1 does not share is flipping with the title's double
    // buffering, or belongs to another object; only what moved is taken from
    // the partner, so the search is run again over that alone.
    for (size_t index = 0; index < after.size(); ++index) {
        if (!movedAt(before, after, index)) {
            m_point[index] = std::numeric_limits<double>::quiet_NaN();
        }
    }
    found = m_frames[0].nearest(key.shader, {m_point, limit, std::nullopt});
    m_partnerCandidates += found.compared;
    return found.entry;
}

std::optional<size_t> ObjectPlanner::nearestEarlier(const AssemblyKey& key,
                                                    std::span<const float> after,
                                                    std::optional<size_t> start) {
    m_point.assign(after.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t index = 0; index < after.size(); ++index) {
        if (isNumber(after[index])) {
            m_point[index] = after[index];
        }
    }
    std::optional<uint32_t> started;
    if (start.has_value()) {
        started = static_cast<uint32_t>(*start);
    }
    DrawTree::Nearest found = m_frames[1].nearest(
        key.shader, {m_point, std::numeric_limits<double>::infinity(), started});
    m_nearestCandidates += found.compared;
    return found.entry;
}

std::optional<size_t> ObjectPlanner::derivedPartner(const AssemblyKey& key,
                                                    std::span<const float> before,
                                                    std::span<const float> after) const {
    const KeyedFrame& between = m_frames[0];
    std::optional<AssemblyKey> derived = derivedKey(key);
    if (!derived.has_value()) {
        return std::nullopt;
    }
    std::optional<size_t> entry = between.find(*derived);
    if (entry.has_value() && between.values(*entry).size() == after.size() &&
        landsOn(measure(before, after, between.values(*entry), frameState(key.shader)))) {
        return entry;
    }
    return std::nullopt;
}

std::optional<ObjectPlanner::Found> ObjectPlanner::findPartner(const AssemblyKey& key,
                                                               std::optional<size_t> earlier,
                                                               std::span<const float> after) {
    const KeyedFrame& between = m_frames[0];
    const KeyedFrame& twoBack = m_frames[1];
    if (earlier.has_value()) {
        if (std::optional<size_t> derived = derivedPartner(key, twoBack.values(*earlier), after)) {
            ++m_partnersDerived;
            return Found{*earlier, *derived};
        }
    }
    uint64_t hash = key.hash();
    // By blocks and by values fail for different objects -- a new one has no
    // blocks to search by -- so each waits on its own failures.
    if (earlier.has_value()) {
        if (waiting(m_searchAgainAt, hash, m_framesPlanned)) {
            ++m_searchesDeferred;
        } else {
            ++m_partnersSearched;
            std::span<const float> before = twoBack.values(*earlier);
            std::optional<size_t> found = searchPartner(key, before, after);
            if (found.has_value() &&
                landsOn(measure(before, after, between.values(*found), frameState(key.shader)))) {
                m_searchAgainAt.erase(hash);
                learn(key, between.key(*found));
                return Found{*earlier, *found};
            }
            m_searchAgainAt.insert_or_assign(hash, m_framesPlanned + kSearchRetryInterval);
        }
    }
    if (waiting(m_reidentifyAgainAt, hash, m_framesPlanned)) {
        ++m_searchesDeferred;
        return std::nullopt;
    }
    // Its blocks named another object in N-2, or none. Not learned: the
    // blocks are what could not be trusted.
    ++m_reidentifyAttempts;
    std::optional<size_t> nearest = nearestEarlier(key, after, earlier);
    if (nearest.has_value() && nearest != earlier) {
        ++m_partnersSearched;
        std::span<const float> before = twoBack.values(*nearest);
        if (equalValues(before, after)) {
            return Found{*nearest, std::nullopt};
        }
        std::optional<size_t> found = searchPartner(key, before, after);
        if (found.has_value() &&
            landsOn(measure(before, after, between.values(*found), frameState(key.shader)))) {
            ++m_partnersReidentified;
            return Found{*nearest, *found};
        }
    }
    m_reidentifyAgainAt.insert_or_assign(hash, m_framesPlanned + kSearchRetryInterval);
    return std::nullopt;
}

ObjectPlanner::Outcome ObjectPlanner::plan(size_t entry) {
    const KeyedFrame& latest = m_building;
    const KeyedFrame& twoBack = m_frames[1];
    const AssemblyKey& key = latest.key(entry);
    std::span<const float> after = latest.values(entry);
    std::optional<size_t> earlier = twoBack.find(key);
    if (earlier.has_value() && twoBack.values(*earlier).size() != after.size()) {
        earlier.reset();
    }
    if (earlier.has_value() && equalValues(twoBack.values(*earlier), after)) {
        // Standing still in its uniforms, it may yet move in the vertices the
        // title poses for it: its draw a frame before is named for that.
        if (std::optional<size_t> partner = derivedPartner(key, twoBack.values(*earlier), after)) {
            m_buildingPlan.partnerAt[entry] = static_cast<uint32_t>(*partner);
            ++m_heldPartnersDerived;
        }
        return Outcome::Held;
    }
    std::optional<Found> found = findPartner(key, earlier, after);
    if (!found.has_value()) {
        return earlier.has_value() ? Outcome::Unverified : Outcome::Unmatched;
    }
    if (!found->partner.has_value()) {
        return Outcome::Held;
    }
    std::span<const float> before = twoBack.values(found->earlier);
    std::span<const float> oneBack = m_frames[0].values(*found->partner);
    std::vector<float>& floats = m_buildingPlan.floats;
    size_t start = floats.size();
    uint64_t notBlended = 0;
    uint64_t alternating = 0;
    for (size_t index = 0; index < after.size(); ++index) {
        float a = oneBack[index];
        float b = after[index];
        // The same at N-2 and N, whatever it was at N-1: not moving, or
        // flipping every frame as the title's double buffering does -- a
        // parity flag averaged is a value the title never wrote.
        if (std::memcmp(&before[index], &b, sizeof(float)) == 0) {
            if (std::memcmp(&a, &b, sizeof(float)) != 0) {
                ++alternating;
            }
            floats.push_back(b);
            continue;
        }
        if (isNumber(a) && isNumber(b)) {
            // Exact where the two agree: a + 0 is a.
            floats.push_back(a + ((b - a) * m_t));
            continue;
        }
        if (a != b) {
            ++notBlended;
        }
        floats.push_back(b);
    }
    std::span<const float> blended(floats.data() + start, after.size());
    if (!liesBetween(before, oneBack, blended, after)) {
        floats.resize(start);
        return Outcome::Outside;
    }
    m_valuesNotBlended += notBlended;
    m_valuesAlternating += alternating;
    m_buildingPlan.blendedAt[entry] = static_cast<uint32_t>(start);
    m_buildingPlan.partnerAt[entry] = static_cast<uint32_t>(*found->partner);
    return Outcome::Blended;
}

std::optional<size_t> ObjectPlanner::partnerOf(size_t entry) const {
    if (entry >= m_plan.partnerAt.size()) {
        return std::nullopt;
    }
    uint32_t at = m_plan.partnerAt[entry];
    if (at == kNotBlended) {
        return std::nullopt;
    }
    return at;
}

std::span<const float> ObjectPlanner::blendOf(size_t entry) const {
    uint32_t at = m_plan.blendedAt[entry];
    if (at == kNotBlended) {
        return {};
    }
    return {m_plan.floats.data() + at, m_frames[0].values(entry).size()};
}

} // namespace wiiuport::interp
