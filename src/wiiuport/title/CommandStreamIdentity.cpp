#include "wiiuport/title/CommandStreamIdentity.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>

namespace wiiuport::title {

void CommandStreamIdentity::dropOverlapping(uintptr_t start, uintptr_t end) {
    for (auto it = m_buffers.begin(); it != m_buffers.end();) {
        const bool same = it->first == start && it->second.end == end;
        const bool overlaps = it->first < end && start < it->second.end;
        if (overlaps && !same) {
            it = m_buffers.erase(it);
            m_report.buffersDropped++;
        } else {
            ++it;
        }
    }
}

void CommandStreamIdentity::bind(uint32_t object) {
    const CommandPosition at = m_readPosition();
    std::scoped_lock lock(m_mutex);
    m_report.binds++;
    if (at.write == 0 || at.write < at.bufferStart || at.write > at.bufferEnd) {
        m_report.bindsWithoutBuffer++;
        return;
    }
    // A command pool reuses its memory in chunks that need not start where the last one did.
    dropOverlapping(at.bufferStart, at.bufferEnd);
    Buffer& buffer = m_buffers[at.bufferStart];
    buffer.end = at.bufferEnd;
    buffer.lastWritten = ++m_writes;
    if (!buffer.binds.empty() && at.write < buffer.binds.back().first) {
        // Written from its start again: the earlier records describe a stream that is gone.
        buffer.binds.clear();
        m_report.buffersRestarted++;
    }
    if (!buffer.binds.empty() && buffer.binds.back().first == at.write) {
        // Nothing was written for the earlier bind, so no packet can belong to it.
        buffer.binds.back().second = object;
    } else {
        buffer.binds.emplace_back(at.write, object);
    }
    if (m_buffers.size() > kBuffers) {
        auto oldest =
            std::min_element(m_buffers.begin(), m_buffers.end(), [](const auto& a, const auto& b) {
                return a.second.lastWritten < b.second.lastWritten;
            });
        m_buffers.erase(oldest);
        m_report.buffersDropped++;
    }
}

uint32_t CommandStreamIdentity::objectAt(uintptr_t packet) const {
    std::scoped_lock lock(m_mutex);
    m_report.lookups++;
    if (packet == 0) {
        m_report.lookupsWithoutPacket++;
        return 0;
    }
    auto buffer = m_buffers.upper_bound(packet);
    if (buffer == m_buffers.begin()) {
        m_report.lookupsWithoutBuffer++;
        return 0;
    }
    --buffer;
    if (packet >= buffer->second.end) {
        m_report.lookupsWithoutBuffer++;
        return 0;
    }
    const auto& binds = buffer->second.binds;
    auto after =
        std::upper_bound(binds.begin(), binds.end(), packet, [](uintptr_t at, const auto& bind) {
            return at < bind.first;
        });
    if (after == binds.begin()) {
        m_report.lookupsBeforeFirstBind++;
        return 0;
    }
    m_report.lookupsNamed++;
    return std::prev(after)->second;
}

CommandStreamIdentity::Report CommandStreamIdentity::report() const {
    std::scoped_lock lock(m_mutex);
    Report out = m_report;
    out.buffersTracked = m_buffers.size();
    return out;
}

std::string CommandStreamIdentity::json() const {
    const Report r = report();
    JsonBody body;
    body.number("binds", r.binds);
    body.number("bindsWithoutBuffer", r.bindsWithoutBuffer);
    body.number("buffersTracked", r.buffersTracked);
    body.number("buffersRestarted", r.buffersRestarted);
    body.number("buffersDropped", r.buffersDropped);
    body.number("lookups", r.lookups);
    body.number("lookupsNamed", r.lookupsNamed);
    body.number("lookupsWithoutPacket", r.lookupsWithoutPacket);
    body.number("lookupsWithoutBuffer", r.lookupsWithoutBuffer);
    body.number("lookupsBeforeFirstBind", r.lookupsBeforeFirstBind);
    body.string("identitySource", "commandStream");
    double share =
        r.lookups == 0 ? 0.0 : static_cast<double>(r.lookupsNamed) / static_cast<double>(r.lookups);
    body.raw("namedShare", JsonBody::real(share, 6));
    return body.finish();
}

} // namespace wiiuport::title
