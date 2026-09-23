#include "check.h"
#include "suites.h"
#include "wiiuport/interp/Blendable.h"
#include "wiiuport/interp/DrawTree.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

using wiiuport::interp::DrawTree;
using wiiuport::interp::DrawValues;
using wiiuport::interp::isNumber;

namespace {

// Fixed, so a failure is the same failure on every run.
constexpr uint32_t kSeed = 0x5e7a11;
constexpr uint32_t kGroups = 24;
constexpr uint32_t kMostDraws = 400;
constexpr uint32_t kQueriesPerGroup = 40;
// Most of an object's values; some draws of a shader have fewer.
constexpr uint32_t kValues = 12;
constexpr uint32_t kShortValues = 9;
// Enough draws that the tree splits them.
constexpr uint32_t kLeafDraws = DrawTree::kLeafDraws;
// Where objects stand, over the first few values; the rest turn and scale
// them, within one.
constexpr uint32_t kPositionValues = 3;
constexpr float kWorld = 1000.0f;
// Relative: a sum of a dozen squares in another order.
constexpr double kSumRounding = 1e-12;

// A frame's draws as KeyedFrame keeps them: one float array, a span per draw.
struct Frame {
    std::vector<float> floats;
    std::vector<std::pair<uint32_t, uint32_t>> spans;
    std::vector<std::vector<uint32_t>> groups;

    DrawValues values() const {
        return DrawValues{floats, spans};
    }
};

// What the title's values hold besides positions: numbers, and the values no
// distance is taken over.
float oddValue(std::mt19937& random) {
    switch (random() % 6) {
    case 0:
        return std::numeric_limits<float>::quiet_NaN();
    case 1:
        return std::numeric_limits<float>::denorm_min();
    case 2:
        return std::numeric_limits<float>::infinity();
    case 3:
        return 0.0f;
    default:
        // Many draws standing in the same place along one value.
        return 1.0f;
    }
}

Frame makeFrame(std::mt19937& random) {
    Frame frame;
    std::uniform_real_distribution<float> world(-kWorld, kWorld);
    std::uniform_real_distribution<float> turn(-1.0f, 1.0f);
    for (uint32_t group = 0; group < kGroups; ++group) {
        uint32_t draws = group == 0 ? 0 : random() % kMostDraws;
        std::vector<uint32_t> entries;
        for (uint32_t draw = 0; draw < draws; ++draw) {
            auto entry = static_cast<uint32_t>(frame.spans.size());
            // Some draws are copies of the one before, as a shader drawing one
            // thing many times makes them.
            if (!entries.empty() && random() % 5 == 0) {
                frame.spans.push_back(frame.spans.back());
                entries.push_back(entry);
                continue;
            }
            uint32_t count = random() % 10 == 0 ? kShortValues : kValues;
            frame.spans.emplace_back(static_cast<uint32_t>(frame.floats.size()), count);
            for (uint32_t at = 0; at < count; ++at) {
                // Some groups draw everything unturned, as a shader drawing
                // tiles does.
                float value = at < kPositionValues ? world(random)
                              : group % 3 == 0     ? 1.0f
                                                   : turn(random);
                frame.floats.push_back(random() % 8 == 0 ? oddValue(random) : value);
            }
            entries.push_back(entry);
        }
        frame.groups.push_back(std::move(entries));
    }
    return frame;
}

// The nearest by comparing every draw: what the tree must find.
std::optional<double> squaredTo(std::span<const double> point, std::span<const float> draw) {
    if (draw.size() != point.size()) {
        return std::nullopt;
    }
    double squared = 0.0;
    for (size_t at = 0; at < point.size(); ++at) {
        if (!std::isnan(point[at]) && isNumber(draw[at])) {
            double off = point[at] - draw[at];
            squared += off * off;
        }
    }
    return squared;
}

std::optional<double> nearestByEveryDraw(const Frame& frame, const std::vector<uint32_t>& group,
                                         const DrawTree::Query& query) {
    std::optional<double> best;
    for (uint32_t entry : group) {
        std::optional<double> squared = squaredTo(query.point, frame.values().of(entry));
        if (squared && *squared <= query.limitSquared && (!best || *squared < *best)) {
            best = squared;
        }
    }
    return best;
}

struct Point {
    std::vector<double> values;
    // Asked near one of the group's draws, as the planner asks, rather than
    // anywhere.
    bool nearADraw;
};

// A point near one of the group's draws, or anywhere, with some values unknown.
Point makePoint(std::mt19937& random, const Frame& frame, const std::vector<uint32_t>& group) {
    std::uniform_real_distribution<double> world(-kWorld, kWorld);
    std::normal_distribution<double> nudge(0.0, 1.0);
    std::optional<uint32_t> near;
    if (!group.empty() && random() % 4 != 0) {
        near = group[random() % group.size()];
    }
    uint32_t count =
        near ? frame.spans[*near].second : (random() % 10 == 0 ? kShortValues : kValues);
    std::vector<double> point(count);
    for (uint32_t at = 0; at < count; ++at) {
        if (random() % 10 == 0) {
            point[at] = std::numeric_limits<double>::quiet_NaN();
        } else if (near) {
            float value = frame.values().of(*near)[at];
            point[at] = (isNumber(value) ? value : 0.0) + nudge(random);
        } else {
            point[at] = world(random);
        }
    }
    return Point{std::move(point), near.has_value()};
}

// Whether the tree names a nearest draw exactly when comparing every draw
// does, and the same one or one as near.
bool agrees(const Frame& frame, const std::vector<uint32_t>& group, const DrawTree::Query& query,
            const DrawTree::Nearest& nearest) {
    std::optional<double> expected = nearestByEveryDraw(frame, group, query);
    if (nearest.entry.has_value() != expected.has_value()) {
        return false;
    }
    if (!expected) {
        return true;
    }
    std::optional<double> actual = squaredTo(query.point, frame.values().of(*nearest.entry));
    // The tree sums in another order than position order, so its own sum may
    // differ in the last bits; the draw it names may not.
    return actual && *actual == *expected &&
           std::abs(nearest.squared - *expected) <= kSumRounding * *expected;
}

void theTreeFindsWhatComparingEveryDrawFinds() {
    std::mt19937 random(kSeed);
    Frame frame = makeFrame(random);
    DrawTree tree;
    std::vector<uint32_t> handles;
    for (const std::vector<uint32_t>& group : frame.groups) {
        handles.push_back(tree.build(group, frame.values()));
    }
    uint64_t queries = 0;
    uint64_t disagreements = 0;
    uint64_t found = 0;
    uint64_t compared = 0;
    uint64_t draws = 0;
    for (size_t group = 0; group < frame.groups.size(); ++group) {
        const std::vector<uint32_t>& entries = frame.groups[group];
        for (uint32_t query = 0; query < kQueriesPerGroup; ++query) {
            Point point = makePoint(random, frame, entries);
            DrawTree::Query asked{point.values};
            if (random() % 3 == 0) {
                asked.limitSquared = std::pow(10.0, random() % 7);
            }
            if (!entries.empty() && random() % 3 == 0) {
                asked.start = entries[random() % entries.size()];
            }
            DrawTree::Nearest nearest = tree.nearest(handles[group], frame.values(), asked);
            ++queries;
            found += nearest.entry.has_value() ? 1 : 0;
            if (!agrees(frame, entries, asked, nearest)) {
                ++disagreements;
            }
            if (point.nearADraw) {
                compared += nearest.compared;
                draws += entries.size();
            }
        }
    }
    check::equal(queries, uint64_t{kGroups} * kQueriesPerGroup, "every query asked");
    check::equal(disagreements, uint64_t{0}, "the tree finds the nearest draw every time");
    // Both answers occur, so agreement is not two searches finding nothing.
    check::isTrue(found > queries / 2 && found < queries,
                  "most queries find a draw and some find none: " + std::to_string(found));
    // What the tree is for: a query near a draw compares a fraction of its
    // group.
    check::isTrue(compared * 4 < draws,
                  "compared " + std::to_string(compared) + " of " + std::to_string(draws));
}

// A group of `count` draws standing about the world, turned, with no odd
// values; `copies` more have the first one's values exactly.
Frame spreadGroup(std::mt19937& random, uint32_t count, uint32_t copies) {
    Frame frame;
    std::uniform_real_distribution<float> world(-kWorld, kWorld);
    std::uniform_real_distribution<float> turn(-1.0f, 1.0f);
    frame.groups.emplace_back();
    for (uint32_t draw = 0; draw < count + copies; ++draw) {
        frame.spans.emplace_back(static_cast<uint32_t>(frame.floats.size()), kValues);
        for (uint32_t at = 0; at < kValues; ++at) {
            frame.floats.push_back(draw >= count          ? frame.floats[at]
                                   : at < kPositionValues ? world(random)
                                                          : turn(random));
        }
        frame.groups[0].push_back(draw);
    }
    return frame;
}

void aHundredCopiesOfOneDrawAreComparedOnce() {
    std::mt19937 random(kSeed + 2);
    Frame frame = spreadGroup(random, 1, 100);
    DrawTree tree;
    uint32_t handle = tree.build(frame.groups[0], frame.values());
    std::vector<double> point(kValues, 2.0 * kWorld);
    DrawTree::Query asked{point};
    DrawTree::Nearest nearest = tree.nearest(handle, frame.values(), asked);
    check::isTrue(agrees(frame, frame.groups[0], asked, nearest), "one of the copies is found");
    check::equal(nearest.compared, uint64_t{1}, "comparing one of them");
}

void aPointFarOutsideTheGroupComparesFewDraws() {
    std::mt19937 random(kSeed + 3);
    Frame frame = spreadGroup(random, kMostDraws, 0);
    DrawTree tree;
    uint32_t handle = tree.build(frame.groups[0], frame.values());
    // Where a draw stands once the camera has moved a long way: far along one
    // value from all of them, near one along the rest.
    std::vector<double> point(frame.values().of(7).begin(), frame.values().of(7).end());
    point[0] += 100.0 * kWorld;
    DrawTree::Query asked{point};
    DrawTree::Nearest nearest = tree.nearest(handle, frame.values(), asked);
    check::isTrue(agrees(frame, frame.groups[0], asked, nearest), "the nearest draw is found");
    check::isTrue(nearest.compared * 10 < kMostDraws,
                  "comparing few: " + std::to_string(nearest.compared));
}

void aViewEveryDrawSharesDoesNotHideTheNearest() {
    std::mt19937 random(kSeed + 5);
    Frame frame = spreadGroup(random, kMostDraws, 0);
    // The latter half of each draw's values is the view the shader is handed,
    // the same for every draw of the frame.
    for (uint32_t draw = 0; draw < kMostDraws; ++draw) {
        for (uint32_t at = kValues / 2; at < kValues; ++at) {
            frame.floats[(draw * kValues) + at] = frame.floats[at];
        }
    }
    DrawTree tree;
    uint32_t handle = tree.build(frame.groups[0], frame.values());
    // Draw 7 a frame later: the camera has moved, so every draw is as far off
    // along the view, and draw 7 alone is near along the rest.
    std::vector<double> point(frame.values().of(7).begin(), frame.values().of(7).end());
    for (uint32_t at = kValues / 2; at < kValues; ++at) {
        point[at] += kWorld;
    }
    DrawTree::Query asked{point};
    DrawTree::Nearest nearest = tree.nearest(handle, frame.values(), asked);
    check::isTrue(agrees(frame, frame.groups[0], asked, nearest), "the nearest draw is found");
    check::isTrue(nearest.entry == std::optional<uint32_t>{7}, "draw 7 is the nearest");
    check::isTrue(nearest.compared * 10 < kMostDraws,
                  "comparing few: " + std::to_string(nearest.compared));
}

void aPointShorterThanEveryDrawFindsNone() {
    std::mt19937 random(kSeed + 4);
    Frame frame = spreadGroup(random, kLeafDraws * 4, 0);
    DrawTree tree;
    uint32_t handle = tree.build(frame.groups[0], frame.values());
    std::vector<double> point(kPositionValues, 0.0);
    DrawTree::Query asked{point};
    check::isTrue(!tree.nearest(handle, frame.values(), asked).entry.has_value(),
                  "no draw has as many values as the point");
}

void aRebuiltTreeForgetsTheFrameBefore() {
    std::mt19937 random(kSeed + 1);
    Frame first = makeFrame(random);
    Frame second = makeFrame(random);
    DrawTree tree;
    tree.build(first.groups[1], first.values());
    tree.clear();
    uint32_t handle = tree.build(second.groups[1], second.values());
    check::equal(handle, uint32_t{0}, "the first group after a clear is the first handle");
    Point point = makePoint(random, second, second.groups[1]);
    DrawTree::Query asked{point.values};
    DrawTree::Nearest nearest = tree.nearest(handle, second.values(), asked);
    check::isTrue(nearest.entry.has_value(), "a draw is found");
    check::isTrue(agrees(second, second.groups[1], asked, nearest), "among the new frame's draws");
}

} // namespace

namespace wiiuport::tests {

void runDrawTreeTests() {
    theTreeFindsWhatComparingEveryDrawFinds();
    aHundredCopiesOfOneDrawAreComparedOnce();
    aPointFarOutsideTheGroupComparesFewDraws();
    aViewEveryDrawSharesDoesNotHideTheNearest();
    aPointShorterThanEveryDrawFindsNone();
    aRebuiltTreeForgetsTheFrameBefore();
}

} // namespace wiiuport::tests
