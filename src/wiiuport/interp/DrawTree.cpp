#include "wiiuport/interp/DrawTree.h"

#include "wiiuport/interp/Blendable.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wiiuport::interp {

namespace {

// How far apart the numbers at one position lie over some draws; zero when
// fewer than two of them are numbers.
float spreadAt(std::span<const uint32_t> entries, uint32_t position, const DrawValues& values) {
    float low = std::numeric_limits<float>::infinity();
    float high = -low;
    for (uint32_t entry : entries) {
        float value = values.of(entry)[position];
        if (isNumber(value)) {
            low = std::min(low, value);
            high = std::max(high, value);
        }
    }
    return high > low ? high - low : 0.0f;
}

// Orders draws by their values' bits, so draws with the same values sit
// together.
bool bitsBefore(std::span<const float> l, std::span<const float> r) {
    if (l.size() != r.size()) {
        return l.size() < r.size();
    }
    return std::memcmp(l.data(), r.data(), l.size_bytes()) < 0;
}

} // namespace

void DrawTree::clear() {
    m_entries.clear();
    m_nodes.clear();
    m_bounds.clear();
    m_groups.clear();
    m_order.clear();
}

uint32_t DrawTree::build(std::span<const uint32_t> group, const DrawValues& values) {
    auto begin = static_cast<uint32_t>(m_entries.size());
    m_entries.insert(m_entries.end(), group.begin(), group.end());
    auto first = m_entries.begin() + begin;
    std::sort(first, m_entries.end(), [&values](uint32_t l, uint32_t r) {
        return bitsBefore(values.of(l), values.of(r));
    });
    m_entries.erase(std::unique(first, m_entries.end(),
                                [&values](uint32_t l, uint32_t r) {
                                    return sameBits(values.of(l), values.of(r));
                                }),
                    m_entries.end());
    auto end = static_cast<uint32_t>(m_entries.size());
    std::span<const uint32_t> distinct(m_entries.data() + begin, end - begin);
    size_t common = distinct.empty() ? 0 : std::numeric_limits<size_t>::max();
    for (uint32_t entry : distinct) {
        common = std::min(common, values.of(entry).size());
    }
    m_spreads.clear();
    for (uint32_t position = 0; position < common; ++position) {
        m_spreads.emplace_back(spreadAt(distinct, position, values), position);
    }
    std::stable_sort(m_spreads.begin(), m_spreads.end(), [](const auto& l, const auto& r) {
        return l.first > r.first;
    });
    Group built{0, static_cast<uint32_t>(m_order.size()), 0, 0, 0};
    for (const auto& [spread, position] : m_spreads) {
        m_order.push_back(position);
        if (spread > 0.0f && built.splitCount < kSplitPositions) {
            ++built.splitCount;
        }
    }
    built.orderEnd = static_cast<uint32_t>(m_order.size());
    built.rest = buildBox(begin, end, built.orderBegin + built.splitCount, built.orderEnd, values);
    built.root = buildNode(begin, end, built, values);
    m_groups.push_back(built);
    return static_cast<uint32_t>(m_groups.size() - 1);
}

uint32_t DrawTree::buildNode(uint32_t begin, uint32_t end, const Group& group,
                             const DrawValues& values) {
    auto index = static_cast<uint32_t>(m_nodes.size());
    m_nodes.push_back(
        Node{begin, end, end, 0, 0, 0,
             buildBox(begin, end, group.orderBegin, group.orderBegin + group.splitCount, values)});
    if (end - begin <= kLeafDraws) {
        return index;
    }
    std::span<const uint32_t> entries(m_entries.data() + begin, end - begin);
    std::optional<uint32_t> position;
    float widest = 0.0f;
    for (uint32_t candidate = 0; candidate < group.splitCount; ++candidate) {
        uint32_t at = m_order[group.orderBegin + candidate];
        float spread = spreadAt(entries, at, values);
        if (spread > widest) {
            widest = spread;
            position = at;
        }
    }
    // Every draw here stands in the same place: nothing to split them by.
    if (!position.has_value()) {
        return index;
    }
    auto valueAt = [&values, at = *position](uint32_t entry) {
        return values.of(entry)[at];
    };
    auto first = m_entries.begin() + begin;
    auto last = m_entries.begin() + end;
    auto numbered = std::partition(first, last, [&](uint32_t entry) {
        return !isNumber(valueAt(entry));
    });
    auto loose = static_cast<uint32_t>(numbered - m_entries.begin());
    uint32_t middle = loose + ((end - loose) / 2);
    std::nth_element(numbered, m_entries.begin() + middle, last, [&](uint32_t l, uint32_t r) {
        return valueAt(l) < valueAt(r);
    });
    uint32_t below = buildNode(loose, middle, group, values);
    uint32_t above = buildNode(middle, end, group, values);
    Node& node = m_nodes[index];
    node.loose = loose;
    node.position = *position;
    node.below = below;
    node.above = above;
    return index;
}

uint32_t DrawTree::buildBox(uint32_t begin, uint32_t end, uint32_t first, uint32_t last,
                            const DrawValues& values) {
    auto box = static_cast<uint32_t>(m_bounds.size());
    for (uint32_t at = first; at < last; ++at) {
        uint32_t position = m_order[at];
        Bounds bounds{std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity()};
        for (uint32_t entry = begin; entry < end; ++entry) {
            float value = values.of(m_entries[entry])[position];
            // A draw with no number here is no distance away along it.
            if (!isNumber(value)) {
                bounds = {-std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::infinity()};
                break;
            }
            bounds.low = std::min(bounds.low, value);
            bounds.high = std::max(bounds.high, value);
        }
        m_bounds.push_back(bounds);
    }
    return box;
}

DrawTree::Nearest DrawTree::nearest(uint32_t group, const DrawValues& values,
                                    const Query& query) const {
    const Group& searched = m_groups[group];
    // Every draw has at least the group's common positions, so a shorter
    // point has as many values as none of them.
    if (query.point.size() < searched.orderEnd - searched.orderBegin) {
        return {};
    }
    Search search(*this, searched, values, query);
    const Node& root = m_nodes[searched.root];
    search.visit(root, search.boxDistance(root));
    return search.result();
}

DrawTree::Search::Search(const DrawTree& tree, const Group& group, const DrawValues& values,
                         const Query& query)
    : m_tree(tree), m_group(group), m_values(values), m_query(query),
      m_bestSquared(query.limitSquared) {
    for (uint32_t at = group.orderBegin + group.splitCount; at < group.orderEnd; ++at) {
        m_restSquared.push_back(
            offBox(tree.m_order[at],
                   tree.m_bounds[group.rest + (at - group.orderBegin - group.splitCount)]));
    }
    if (query.start.has_value()) {
        ++m_found.compared;
        if (std::optional<double> squared = distance(*query.start)) {
            m_found.entry = query.start;
            m_found.squared = *squared;
            m_bestSquared = *squared;
        }
    }
}

std::optional<double> DrawTree::Search::distance(uint32_t entry) const {
    std::span<const float> candidate = m_values.of(entry);
    std::span<const double> point = m_query.point;
    if (candidate.size() != point.size()) {
        return std::nullopt;
    }
    double squared = 0.0;
    auto beaten = [&](size_t position) {
        if (!std::isnan(point[position]) && isNumber(candidate[position])) {
            double off = point[position] - candidate[position];
            squared += off * off;
        }
        return !couldBeFound(squared);
    };
    for (uint32_t at = m_group.orderBegin; at < m_group.orderEnd; ++at) {
        if (beaten(m_tree.m_order[at])) {
            return std::nullopt;
        }
    }
    for (size_t position = m_group.orderEnd - m_group.orderBegin; position < point.size();
         ++position) {
        if (beaten(position)) {
            return std::nullopt;
        }
    }
    return squared;
}

double DrawTree::Search::offBox(uint32_t position, const Bounds& bounds) const {
    // NaN compares false either way: a position the point has no value at is
    // no distance off.
    double value = m_query.point[position];
    if (value < bounds.low) {
        double off = value - bounds.low;
        return off * off;
    }
    if (value > bounds.high) {
        double off = value - bounds.high;
        return off * off;
    }
    return 0.0;
}

double DrawTree::Search::boxDistance(const Node& node) const {
    // The positions in the order distance() sums them, each no further than
    // the draw's own, so a box's sum is never more than a draw's inside it,
    // rounding included.
    double squared = 0.0;
    for (uint32_t split = 0; split < m_group.splitCount; ++split) {
        squared +=
            offBox(m_tree.m_order[m_group.orderBegin + split], m_tree.m_bounds[node.box + split]);
    }
    for (double rest : m_restSquared) {
        squared += rest;
    }
    return squared;
}

bool DrawTree::Search::couldBeFound(double squared) const {
    // Once a draw is found, one only as near is not nearer.
    return m_found.entry.has_value() ? squared < m_bestSquared : squared <= m_bestSquared;
}

void DrawTree::Search::consider(uint32_t entry) {
    if (entry == m_query.start) {
        return;
    }
    ++m_found.compared;
    if (std::optional<double> squared = distance(entry)) {
        m_found.entry = entry;
        m_found.squared = *squared;
        m_bestSquared = *squared;
    }
}

void DrawTree::Search::visit(const Node& node, double boxSquared) {
    if (!couldBeFound(boxSquared)) {
        return;
    }
    for (uint32_t at = node.begin; at < node.loose; ++at) {
        consider(m_tree.m_entries[at]);
    }
    if (node.loose == node.end) {
        return;
    }
    // The nearer box first: the draw found there bounds the other.
    const Node& belowNode = m_tree.m_nodes[node.below];
    const Node& aboveNode = m_tree.m_nodes[node.above];
    double below = boxDistance(belowNode);
    double above = boxDistance(aboveNode);
    if (below <= above) {
        visit(belowNode, below);
        visit(aboveNode, above);
    } else {
        visit(aboveNode, above);
        visit(belowNode, below);
    }
}

} // namespace wiiuport::interp
