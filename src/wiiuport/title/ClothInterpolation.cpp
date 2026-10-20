#include "wiiuport/title/ClothInterpolation.h"

#include "wiiuport/guest/ProbeInstallation.h"
#include "wiiuport/interp/Blendable.h"
#include "wiiuport/title/JsonBody.h"

#include <bit>
#include <utility>

namespace wiiuport::title {

void ClothInterpolation::Entry::OnInstall(GuestCallProbes::Installation value) {
    std::scoped_lock lock(m_owner.m_mutex);
    installation = value;
}

void ClothInterpolation::Entry::OnCall(std::span<const uint32_t, 32> gpr,
                                       uint32_t /*returnAddress*/) {
    m_owner.onUpload(gpr[kPacketRegister]);
}

ClothInterpolation::ClothInterpolation(Seams seams)
    : m_register(seams.registerProbe), m_readWords(std::move(seams.readWords)),
      m_writeWords(std::move(seams.writeWords)) {
}

void ClothInterpolation::install() {
    m_register(kUpload, kUploadFirst, m_upload, true, 0);
}

void ClothInterpolation::onUpload(uint32_t packet) {
    std::scoped_lock lock(m_mutex);
    m_uploads++;
    Seen current{.tick = m_tick};
    if (!read(packet, current)) {
        m_unreadable++;
        m_seen.erase(packet);
        return;
    }
    if (!m_inDrawPhase) {
        m_seen[packet] = std::move(current);
        return;
    }
    auto previous = m_seen.find(packet);
    if (previous == m_seen.end() || previous->second.tick + 1 != m_tick ||
        previous->second.words.front().size() != current.words.front().size()) {
        m_firstSeen++;
        return;
    }
    if (m_blended.contains(current.buffers.front())) {
        m_shared++;
        return;
    }
    blend(previous->second, current);
}

bool ClothInterpolation::read(uint32_t packet, Seen& seen) {
    uint32_t fly = 0;
    uint32_t hoist = 0;
    uint32_t current = 0;
    std::vector<uint32_t> buffers(size_t{kBufferKinds} * 2);
    if (!m_readWords(packet + kFlyGrid, &fly, 1) || !m_readWords(packet + kHoistGrid, &hoist, 1) ||
        !m_readWords(packet + kCurrent, &current, 1) ||
        !m_readWords(packet + kBuffers, buffers.data(), static_cast<uint32_t>(buffers.size()))) {
        return false;
    }
    if (fly == 0 || hoist == 0 || fly * hoist > kMaxPoints) {
        return false;
    }
    uint32_t index = (current >> 24) & 1;
    uint32_t words = fly * hoist * 3;
    for (uint32_t kind = 0; kind < kBufferKinds; ++kind) {
        uint32_t buffer = buffers[(kind * 2) + index];
        std::vector<uint32_t> values(words);
        if (buffer == 0 || !m_readWords(buffer, values.data(), words)) {
            return false;
        }
        seen.buffers.push_back(buffer);
        seen.words.push_back(std::move(values));
    }
    return true;
}

void ClothInterpolation::blend(const Seen& previous, const Seen& current) {
    std::vector<std::vector<uint32_t>> blended;
    for (uint32_t kind = 0; kind < kBufferKinds; ++kind) {
        const std::vector<uint32_t>& from = previous.words[kind];
        const std::vector<uint32_t>& to = current.words[kind];
        std::vector<float> a(to.size());
        std::vector<float> b(to.size());
        std::vector<float> mid(to.size());
        for (size_t i = 0; i < to.size(); ++i) {
            a[i] = std::bit_cast<float>(from[i]);
            b[i] = std::bit_cast<float>(to[i]);
        }
        if (!interp::midpoint(a, b, mid)) {
            m_unblendable++;
            return;
        }
        std::vector<uint32_t> words(to.size());
        for (size_t i = 0; i < mid.size(); ++i) {
            words[i] = std::bit_cast<uint32_t>(mid[i]);
        }
        blended.push_back(std::move(words));
    }
    for (uint32_t kind = 0; kind < kBufferKinds; ++kind) {
        uint32_t buffer = current.buffers[kind];
        const std::vector<uint32_t>& words = blended[kind];
        if (!m_writeWords(buffer, words.data(), static_cast<uint32_t>(words.size()))) {
            m_writeFailures++;
            continue;
        }
        m_restores.push_back({.address = buffer, .words = current.words[kind]});
        m_blended.insert(buffer);
    }
    m_blends++;
}

void ClothInterpolation::onDrawPhaseBegin(uint64_t tick) {
    std::scoped_lock lock(m_mutex);
    m_inDrawPhase = true;
    m_tick = tick;
}

void ClothInterpolation::onDrawPhaseEnd() {
    std::scoped_lock lock(m_mutex);
    m_inDrawPhase = false;
    for (const Restore& restore : m_restores) {
        if (m_writeWords(restore.address, restore.words.data(),
                         static_cast<uint32_t>(restore.words.size()))) {
            m_restored++;
        } else {
            m_writeFailures++;
        }
    }
    m_restores.clear();
    m_blended.clear();
}

void ClothInterpolation::onActorDraw(uint32_t /*actor*/) {
}

std::string ClothInterpolation::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("upload", std::string(guest::installationName(m_upload.installation)));
    body.number("uploads", m_uploads);
    body.number("blends", m_blends);
    body.number("firstSeen", m_firstSeen);
    body.number("shared", m_shared);
    body.number("unblendable", m_unblendable);
    body.number("unreadable", m_unreadable);
    body.number("writeFailures", m_writeFailures);
    body.number("restored", m_restored);
    return body.finish();
}

} // namespace wiiuport::title
