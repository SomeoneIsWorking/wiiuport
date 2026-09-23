#include "wiiuport/interp/ObjectCensus.h"

#include <algorithm>
#include <stdexcept>

namespace wiiuport::interp {

uint64_t ObjectCensus::Row::unblended() const {
    using Outcome = ObjectPlanner::Outcome;
    return outcomes[static_cast<size_t>(Outcome::Unmatched)] +
           outcomes[static_cast<size_t>(Outcome::Unverified)] +
           outcomes[static_cast<size_t>(Outcome::Outside)];
}

void CensusTally::add(const ObjectPlanner& planner) {
    if (!planner.ready()) {
        throw std::logic_error("CensusTally: the planner's latest frame was not planned");
    }
    const KeyedFrame& frame = planner.latest();
    for (size_t entry = 0; entry < frame.size(); ++entry) {
        const AssemblyKey& key = frame.key(entry);
        ObjectCensus::Row& row = m_byShader[key.shader];
        row.shader = key.shader;
        ++row.draws;
        ++row.outcomes[static_cast<size_t>(planner.outcomeOf(entry))];
        row.mostValues = std::max<uint64_t>(row.mostValues, frame.values(entry).size());
        if (key.sourceCount == 0) {
            ++row.withoutBlocks;
        }
    }
    ++m_frames;
}

ObjectCensus CensusTally::census() const {
    ObjectCensus census;
    census.frames = m_frames;
    census.shaders = m_byShader.size();
    census.rows.reserve(m_byShader.size());
    for (const auto& [shader, row] : m_byShader) {
        census.objects += row.draws;
        for (size_t index = 0; index < ObjectPlanner::kOutcomeCount; ++index) {
            census.outcomes[index] += row.outcomes[index];
        }
        census.rows.push_back(row);
    }
    std::stable_sort(census.rows.begin(), census.rows.end(),
                     [](const ObjectCensus::Row& l, const ObjectCensus::Row& r) {
                         return l.unblended() > r.unblended();
                     });
    census.rows.resize(std::min(census.rows.size(), ObjectCensus::kMaxRows));
    return census;
}

} // namespace wiiuport::interp
