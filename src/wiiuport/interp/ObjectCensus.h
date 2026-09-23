#pragma once

#include "wiiuport/interp/ObjectPlanner.h"
#include "wiiuport/interp/TransformSearch.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace wiiuport::interp {

// What planned frames' objects came to, shader by shader.
//
// The outcome totals say how many objects were drawn un-blended; this says
// which: a shader whose every draw is unverified is a kind of object the blend
// does not understand, and one whose draws are mostly blended with a few
// unmatched is objects entering and leaving.
struct ObjectCensus {
    struct Row {
        ShaderKey shader;
        uint64_t draws{0};
        ObjectPlanner::Outcomes outcomes{};
        // The widest draw's values, and draws that sourced no uniform block
        // and so are known only by their place in the draw order.
        uint64_t mostValues{0};
        uint64_t withoutBlocks{0};

        // Drawn as the title drew it although it may have moved.
        uint64_t unblended() const;
    };

    // Rows kept, most un-blended first; the rest are only counted.
    static constexpr size_t kMaxRows = 24;

    uint64_t frames{0};
    uint64_t objects{0};
    ObjectPlanner::Outcomes outcomes{};
    uint64_t shaders{0};
    std::vector<Row> rows;
};

// Adds planned frames into one census. Pure.
class CensusTally {
  public:
    // Throws std::logic_error unless the planner is ready: a frame that was
    // never planned would count every object unmatched.
    void add(const ObjectPlanner& planner);

    uint64_t frames() const {
        return m_frames;
    }

    ObjectCensus census() const;

  private:
    uint64_t m_frames{0};
    std::map<ShaderKey, ObjectCensus::Row> m_byShader;
};

} // namespace wiiuport::interp
