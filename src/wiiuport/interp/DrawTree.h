#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace wiiuport::interp {

// Where one frame's draws keep their values: entry e's are `spans[e].second`
// floats from `floats[spans[e].first]`.
struct DrawValues {
    std::span<const float> floats;
    std::span<const std::pair<uint32_t, uint32_t>> spans;

    std::span<const float> of(uint32_t entry) const {
        return floats.subspan(spans[entry].first, spans[entry].second);
    }
};

// Finds, among one group of a frame's draws, the draw nearest a point: the
// squared distance over the positions where the point has a value and the
// draw a number, among draws with as many values as the point. Exact.
//
// A k-d tree over the few values that spread the group widest. An object's
// values are dozens of floats, but what tells one object from another is a
// handful of them -- where it stands -- and a search ordered by a single value
// compared some two hundred draws a query on the sea. Built once per frame
// and searched from the frames after it.
//
// A node is passed over when the box its draws fill over those values is
// already further than the nearest draw found: a query far outside the whole
// group, as every object is once the camera has turned, is then as cheap as
// one inside it, which a bound on the split value alone does not make it.
// Draws with the same values are one draw to the tree: a shader drawing a
// hundred copies of one thing would otherwise compare all hundred.
//
// The bound also holds the positions nodes do not split on, over the whole
// group's box: values every draw shares -- the view each of a shader's draws
// is handed -- are as far from a point taken a frame later for every draw, and
// a bound without them is below every draw's distance once the camera moves,
// so nothing is passed over.
class DrawTree {
  public:
    // How many of a group's widest-spread positions its nodes may split on.
    static constexpr size_t kSplitPositions = 6;
    // A node with this many draws or fewer is compared draw by draw.
    static constexpr uint32_t kLeafDraws = 8;

    struct Query {
        // NaN where the point has no value to compare.
        std::span<const double> point;
        // A draw further than this is not found; one exactly at it is.
        double limitSquared{std::numeric_limits<double>::infinity()};
        // A draw known to be near, which the search then only has to beat.
        std::optional<uint32_t> start;
    };

    struct Nearest {
        std::optional<uint32_t> entry;
        double squared{0.0};
        // Draws whose values were compared with the point.
        uint64_t compared{0};
    };

    // Drops every group, keeping the storage.
    void clear();
    // Builds a tree over one group's entries and returns the group's handle.
    uint32_t build(std::span<const uint32_t> group, const DrawValues& values);
    Nearest nearest(uint32_t group, const DrawValues& values, const Query& query) const;

  private:
    struct Node {
        // m_entries[begin, end): [begin, loose) have no number at `position`
        // and are compared whenever the node is; a leaf has loose == end.
        uint32_t begin;
        uint32_t loose;
        uint32_t end;
        uint32_t position;
        // Children: values at `position` no greater than the split, and no
        // less.
        uint32_t below;
        uint32_t above;
        // Where the node's box starts in m_bounds: the lowest and highest
        // value its draws have at each of the group's split positions, or
        // the whole line where one of them has no number there.
        uint32_t box;
    };

    struct Bounds {
        float low;
        float high;
    };

    struct Group {
        uint32_t root;
        // Every position all the group's draws have, widest spread first: the
        // order a distance to a draw grows fastest in, so a draw that is not
        // the nearest is abandoned after a few.
        uint32_t orderBegin;
        uint32_t orderEnd;
        // The positions nodes may split on, in m_order.
        uint32_t splitCount;
        // Where the whole group's box over its other positions starts in
        // m_bounds, in m_order's order after the split positions.
        uint32_t rest;
    };

    uint32_t buildNode(uint32_t begin, uint32_t end, const Group& group, const DrawValues& values);
    // The box of m_entries[begin, end) over m_order[first, last), appended
    // to m_bounds.
    uint32_t buildBox(uint32_t begin, uint32_t end, uint32_t first, uint32_t last,
                      const DrawValues& values);

    // One query's state, walked down a group's tree.
    class Search {
      public:
        Search(const DrawTree& tree, const Group& group, const DrawValues& values,
               const Query& query);
        // Searches a node whose box is `boxSquared` from the point.
        void visit(uint32_t node, double boxSquared);
        // The squared distance to a node's box: no draw in it is nearer.
        double boxDistance(const Node& node) const;

        Nearest result() const {
            return m_found;
        }

      private:
        // The draw's squared distance, or nothing once it cannot be nearer.
        std::optional<double> distance(uint32_t entry) const;
        // Whether a draw this far away could still be the one found.
        bool couldBeFound(double squared) const;
        void consider(uint32_t entry);
        // How far the point is from the group's box at one position.
        double offBox(uint32_t position, const Bounds& bounds) const;

        const DrawTree& m_tree;
        const Group& m_group;
        const DrawValues& m_values;
        const Query& m_query;
        double m_bestSquared;
        Nearest m_found;
        // The point's squared distance to the group's box at each position
        // after the split ones, summed after them as distance() sums them.
        std::vector<double> m_restSquared;
    };

    std::vector<uint32_t> m_entries;
    std::vector<Node> m_nodes;
    std::vector<Bounds> m_bounds;
    std::vector<Group> m_groups;
    std::vector<uint32_t> m_order;
    // Reused by build(): each position's spread.
    std::vector<std::pair<float, uint32_t>> m_spreads;
};

} // namespace wiiuport::interp
