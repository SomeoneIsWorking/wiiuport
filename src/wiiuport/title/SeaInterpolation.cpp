#include "wiiuport/title/SeaInterpolation.h"

#include "wiiuport/interp/Blendable.h"
#include "wiiuport/title/JsonBody.h"

#include <bit>
#include <cmath>
#include <utility>

namespace wiiuport::title {

namespace {

float asFloat(uint32_t word) {
    return std::bit_cast<float>(word);
}

} // namespace

SeaInterpolation::SeaInterpolation(Seams seams)
    : m_readWords(std::move(seams.readWords)), m_writeWords(std::move(seams.writeWords)) {
}

void SeaInterpolation::onDrawPhaseBegin(uint64_t tick) {
    std::scoped_lock lock(m_mutex);
    uint32_t packet = 0;
    uint32_t flags = 0;
    if (!m_readWords(kSeaPacket, &packet, 1)) {
        m_unreadable++;
        return;
    }
    if (packet == 0) {
        return;
    }
    uint32_t counter = 0;
    if (!m_readWords(packet + kInitFlag, &flags, 1) ||
        !m_readWords(packet + kAnimCounter, &counter, 1)) {
        m_unreadable++;
        return;
    }
    // daSea_Draw steps the counter, culled or not; the skipped call's draw phase steps it again.
    m_restores.push_back({.address = packet + kAnimCounter, .words = {counter}});
    m_counterHolds++;
    bool initialised = (flags >> 24) != 0;
    bool culled = ((flags >> 16) & 0xff) == 1;
    if (!initialised || culled) {
        m_seen = {};
        return;
    }
    Seen current{.tick = tick, .words = std::vector<uint32_t>(2 + kHeights)};
    if (!m_readWords(packet + kHeightTable, &current.heightTable, 1) || current.heightTable == 0 ||
        !m_readWords(packet + kDrawMin, current.words.data(), 2) ||
        !m_readWords(current.heightTable, current.words.data() + 2, kHeights)) {
        m_unreadable++;
        return;
    }
    Seen previous = std::exchange(m_seen, current);
    if (previous.tick + 1 != tick || previous.heightTable != current.heightTable) {
        m_firstSeen++;
        return;
    }
    blend(packet, previous, current);
}

void SeaInterpolation::blend(uint32_t packet, const Seen& previous, const Seen& current) {
    float dx = asFloat(current.words[0]) - asFloat(previous.words[0]);
    float dz = asFloat(current.words[1]) - asFloat(previous.words[1]);
    if (!(std::hypot(dx, dz) < kMaxStep)) {
        m_warps++;
        return;
    }
    std::vector<float> from(current.words.size());
    std::vector<float> to(current.words.size());
    std::vector<float> mid(current.words.size());
    for (size_t i = 0; i < current.words.size(); ++i) {
        from[i] = asFloat(previous.words[i]);
        to[i] = asFloat(current.words[i]);
    }
    if (!interp::midpoint(from, to, mid)) {
        m_unblendable++;
        return;
    }
    std::vector<uint32_t> blended(mid.size());
    for (size_t i = 0; i < mid.size(); ++i) {
        blended[i] = std::bit_cast<uint32_t>(mid[i]);
    }
    write(packet + kDrawMin, {.blended = {blended.begin(), blended.begin() + 2},
                              .original = {current.words.begin(), current.words.begin() + 2}});
    write(current.heightTable, {.blended = {blended.begin() + 2, blended.end()},
                                .original = {current.words.begin() + 2, current.words.end()}});
    m_blends++;
}

void SeaInterpolation::write(uint32_t address, Change change) {
    if (!m_writeWords(address, change.blended.data(),
                      static_cast<uint32_t>(change.blended.size()))) {
        m_writeFailures++;
        return;
    }
    m_restores.push_back({.address = address, .words = std::move(change.original)});
}

void SeaInterpolation::onDrawPhaseEnd() {
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
}

std::string SeaInterpolation::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.number("blends", m_blends);
    body.number("firstSeen", m_firstSeen);
    body.number("warps", m_warps);
    body.number("counterHolds", m_counterHolds);
    body.number("unblendable", m_unblendable);
    body.number("unreadable", m_unreadable);
    body.number("writeFailures", m_writeFailures);
    body.number("restored", m_restored);
    return body.finish();
}

} // namespace wiiuport::title
