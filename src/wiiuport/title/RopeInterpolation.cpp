#include "wiiuport/title/RopeInterpolation.h"

#include "wiiuport/interp/Blendable.h"
#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace wiiuport::title {

namespace {

// A point's three float words in the previous tick and this one.
struct Ends {
    const uint32_t* from;
    const uint32_t* to;
};

// The midpoint of a point into `out`; whether all three words could be blended.
bool midpointWords(Ends ends, uint32_t* out) {
    std::array<float, 3> l{};
    std::array<float, 3> r{};
    std::array<float, 3> mid{};
    for (size_t axis = 0; axis < 3; ++axis) {
        l.at(axis) = std::bit_cast<float>(ends.from[axis]);
        r.at(axis) = std::bit_cast<float>(ends.to[axis]);
    }
    if (!interp::midpoint(l, r, mid)) {
        return false;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        out[axis] = std::bit_cast<uint32_t>(mid.at(axis));
    }
    return true;
}

} // namespace

RopeInterpolation::RopeInterpolation(Seams seams)
    : m_readWords(std::move(seams.readWords)), m_writeWords(std::move(seams.writeWords)) {
}

void RopeInterpolation::onDrawPhaseBegin(uint64_t tick) {
    std::scoped_lock lock(m_mutex);
    m_tick = tick;
}

void RopeInterpolation::onActorDraw(uint32_t actor) {
    std::scoped_lock lock(m_mutex);
    uint32_t name = 0;
    if (!m_readWords(actor + kProcessName, &name, 1)) {
        m_unreadable++;
        return;
    }
    if ((name >> 16) != kHimo2) {
        return;
    }
    m_draws++;
    // The draw eases the sway from where its last draw left it; the skipped call's draw does it.
    hold(actor + kSway, kSwayWords);
    hold(actor + kTilt, 1);
    Seen current{.tick = m_tick};
    if (!read(actor, current)) {
        m_unreadable++;
        m_seen.erase(actor);
        return;
    }
    Seen previous = std::exchange(m_seen[actor], current);
    if (previous.tick == 0 || previous.tick + 1 != m_tick) {
        m_firstSeen++;
        return;
    }
    if (blend(actor, previous, current)) {
        m_blends++;
    } else {
        m_unblendable++;
    }
}

bool RopeInterpolation::read(uint32_t actor, Seen& seen) {
    seen.segments.resize(kSegmentWords);
    return m_readWords(actor + kFirstSegment, &seen.firstSegment, 1) &&
           seen.firstSegment <= kSegmentCount &&
           m_readWords(actor + kRopeEnd, seen.end.data(), 3) &&
           m_readWords(actor + kHandHook, seen.handHook.data(), kHandHookWords) &&
           m_readWords(actor + kSegments, seen.segments.data(), kSegmentWords);
}

bool RopeInterpolation::blend(uint32_t actor, const Seen& previous, const Seen& current) {
    std::vector<uint32_t> end(3);
    std::vector<uint32_t> handHook(kHandHookWords);
    std::vector<uint32_t> segments = current.segments;
    if (!midpointWords({previous.end.data(), current.end.data()}, end.data()) ||
        !midpointWords({previous.handHook.data(), current.handHook.data()}, handHook.data()) ||
        !midpointWords({previous.handHook.data() + 3, current.handHook.data() + 3},
                       handHook.data() + 3)) {
        return false;
    }
    // A segment either tick had not paid out has nowhere to come from.
    for (uint32_t i = std::max(previous.firstSegment, current.firstSegment); i < kSegmentCount;
         ++i) {
        size_t word = ((i * kSegmentStride) + kSegmentPosition) / 4;
        if (!midpointWords({previous.segments.data() + word, current.segments.data() + word},
                           segments.data() + word)) {
            return false;
        }
    }
    write(actor + kRopeEnd,
          {.blended = std::move(end), .original = {current.end.begin(), current.end.end()}});
    write(actor + kHandHook, {.blended = std::move(handHook),
                              .original = {current.handHook.begin(), current.handHook.end()}});
    write(actor + kSegments, {.blended = std::move(segments), .original = current.segments});
    return true;
}

void RopeInterpolation::hold(uint32_t address, uint32_t count) {
    std::vector<uint32_t> words(count);
    if (!m_readWords(address, words.data(), count)) {
        m_unreadable++;
        return;
    }
    m_restores.push_back({.address = address, .words = std::move(words)});
}

void RopeInterpolation::write(uint32_t address, Change change) {
    if (!m_writeWords(address, change.blended.data(),
                      static_cast<uint32_t>(change.blended.size()))) {
        m_writeFailures++;
        return;
    }
    m_restores.push_back({.address = address, .words = std::move(change.original)});
}

void RopeInterpolation::onDrawPhaseEnd() {
    std::scoped_lock lock(m_mutex);
    for (const Restore& restore : m_restores) {
        if (m_writeWords(restore.address, restore.words.data(),
                         static_cast<uint32_t>(restore.words.size()))) {
            m_restored++;
        } else {
            m_writeFailures++;
        }
    }
    m_restores.clear();
    std::erase_if(m_seen, [this](const auto& entry) {
        return entry.second.tick < m_tick;
    });
}

std::string RopeInterpolation::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.number("draws", m_draws);
    body.number("blends", m_blends);
    body.number("firstSeen", m_firstSeen);
    body.number("unblendable", m_unblendable);
    body.number("unreadable", m_unreadable);
    body.number("writeFailures", m_writeFailures);
    body.number("restored", m_restored);
    return body.finish();
}

} // namespace wiiuport::title
