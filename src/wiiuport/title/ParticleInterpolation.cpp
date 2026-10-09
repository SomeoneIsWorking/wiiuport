#include "wiiuport/title/ParticleInterpolation.h"

#include "wiiuport/guest/ProbeInstallation.h"
#include "wiiuport/interp/Blendable.h"
#include "wiiuport/title/JsonBody.h"

#include <bit>
#include <utility>

namespace wiiuport::title {

void ParticleInterpolation::Entry::OnInstall(GuestCallProbes::Installation value) {
    std::scoped_lock lock(m_owner.m_mutex);
    installation = value;
}

void ParticleInterpolation::Entry::OnCall(std::span<const uint32_t, 32> gpr,
                                          uint32_t /*returnAddress*/) {
    m_owner.onEmitterDraw(gpr[kDrawRegister]);
}

ParticleInterpolation::ParticleInterpolation(Seams seams)
    : m_register(seams.registerProbe), m_readWords(std::move(seams.readWords)),
      m_writeWords(std::move(seams.writeWords)) {
}

void ParticleInterpolation::install() {
    m_register(kEmitterDraw, kEmitterDrawFirst, m_emitterDraw, true, 0);
}

void ParticleInterpolation::onEmitterDraw(uint32_t draw) {
    std::scoped_lock lock(m_mutex);
    m_emitterDraws++;
    std::optional<std::vector<uint32_t>> particles = particlesOf(draw);
    if (!particles) {
        m_unreadable++;
        return;
    }
    for (uint32_t particle : *particles) {
        if (m_inMidPaint) {
            blend(particle);
        } else {
            record(particle);
        }
    }
}

std::optional<std::vector<uint32_t>> ParticleInterpolation::particlesOf(uint32_t draw) {
    uint32_t emitter = 0;
    if (!m_readWords(draw + kDrawEmitter, &emitter, 1) || emitter == 0) {
        return std::nullopt;
    }
    std::vector<uint32_t> particles;
    for (uint32_t list : kParticleLists) {
        uint32_t link = 0;
        if (!m_readWords(emitter + list, &link, 1)) {
            return std::nullopt;
        }
        for (uint32_t length = 0; link != 0; ++length) {
            std::array<uint32_t, (kLinkNext / 4) + 1> words{};
            if (length == kMaxListLength ||
                !m_readWords(link, words.data(), static_cast<uint32_t>(words.size()))) {
                return std::nullopt;
            }
            particles.push_back(words.at(kLinkObject / 4));
            link = words.at(kLinkNext / 4);
        }
    }
    return particles;
}

// A paint that draws the tick as it is: where each particle was drawn, for the next mid paint.
void ParticleInterpolation::record(uint32_t particle) {
    Seen seen{.tick = m_tick};
    uint32_t age = 0;
    if (!m_readWords(particle + kGlobalPosition, seen.position.data(), kPositionWords) ||
        !m_readWords(particle + kAge, &age, 1)) {
        m_unreadable++;
        return;
    }
    seen.age = std::bit_cast<float>(age);
    m_seen[particle] = seen;
}

void ParticleInterpolation::blend(uint32_t particle) {
    if (!m_blended.insert(particle).second) {
        // A second draw of the emitter in this paint: already at its midpoint.
        return;
    }
    std::array<uint32_t, kPositionWords> position{};
    uint32_t ageWord = 0;
    if (!m_readWords(particle + kGlobalPosition, position.data(), kPositionWords) ||
        !m_readWords(particle + kAge, &ageWord, 1)) {
        m_unreadable++;
        return;
    }
    auto seen = m_seen.find(particle);
    if (seen == m_seen.end() || seen->second.tick + 1 != m_tick) {
        m_particleFirstSeen++;
        return;
    }
    if (!(std::bit_cast<float>(ageWord) > seen->second.age)) {
        // The pool reused the slot for a new particle.
        m_particlesRenewed++;
        return;
    }
    std::array<float, kPositionWords> from{};
    std::array<float, kPositionWords> to{};
    std::array<float, kPositionWords> mid{};
    for (size_t axis = 0; axis < kPositionWords; ++axis) {
        from.at(axis) = std::bit_cast<float>(seen->second.position.at(axis));
        to.at(axis) = std::bit_cast<float>(position.at(axis));
    }
    if (!interp::midpoint(from, to, mid)) {
        m_unblendable++;
        return;
    }
    std::array<uint32_t, kPositionWords> blended{};
    for (size_t axis = 0; axis < kPositionWords; ++axis) {
        blended.at(axis) = std::bit_cast<uint32_t>(mid.at(axis));
    }
    if (!m_writeWords(particle + kGlobalPosition, blended.data(), kPositionWords)) {
        m_writeFailures++;
        return;
    }
    m_restores.push_back({.particle = particle, .position = position});
    m_particleBlends++;
}

void ParticleInterpolation::onMidPaintBegin(uint64_t tick) {
    std::scoped_lock lock(m_mutex);
    m_inMidPaint = true;
    m_tick = tick;
    m_blended.clear();
}

void ParticleInterpolation::onMidPaintEnd() {
    std::scoped_lock lock(m_mutex);
    m_inMidPaint = false;
    for (const Restore& restore : m_restores) {
        if (m_writeWords(restore.particle + kGlobalPosition, restore.position.data(),
                         kPositionWords)) {
            m_restored++;
        } else {
            m_writeFailures++;
        }
    }
    m_restores.clear();
    std::erase_if(m_seen, [this](const auto& entry) {
        return entry.second.tick + 1 < m_tick;
    });
}

std::string ParticleInterpolation::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("emitterDraw", std::string(guest::installationName(m_emitterDraw.installation)));
    body.number("emitterDraws", m_emitterDraws);
    body.number("particleBlends", m_particleBlends);
    body.number("particleFirstSeen", m_particleFirstSeen);
    body.number("particlesRenewed", m_particlesRenewed);
    body.number("unblendable", m_unblendable);
    body.number("unreadable", m_unreadable);
    body.number("writeFailures", m_writeFailures);
    body.number("restored", m_restored);
    return body.finish();
}

} // namespace wiiuport::title
