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

namespace {

float asFloat(uint32_t word) {
    return std::bit_cast<float>(word);
}

// The same words at the last tick and now.
struct Span {
    std::span<const uint32_t> from;
    std::span<const uint32_t> to;
};

// The midpoint of each float word, into `out`; false when one is not a number.
bool blendFloats(Span words, std::span<uint32_t> out) {
    for (size_t i = 0; i < words.from.size(); ++i) {
        std::array<float, 1> a{asFloat(words.from[i])};
        std::array<float, 1> b{asFloat(words.to[i])};
        std::array<float, 1> mid{};
        if (!interp::midpoint(a, b, mid)) {
            return false;
        }
        out[i] = std::bit_cast<uint32_t>(mid[0]);
    }
    return true;
}

// Each byte of an RGBA8 colour halfway between its two words.
uint32_t midpointColor(Span colour) {
    uint32_t out = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        uint32_t a = (colour.from[0] >> shift) & 0xff;
        uint32_t b = (colour.to[0] >> shift) & 0xff;
        out |= ((a + b) / 2) << shift;
    }
    return out;
}

} // namespace

bool ParticleInterpolation::read(uint32_t particle, Drawn& drawn, uint32_t& age) {
    return m_readWords(particle + kGlobalPosition, drawn.position.data(), kPositionWords) &&
           m_readWords(particle + kDrawParams, drawn.params.data(), kDrawParamWords) &&
           m_readWords(particle + kAge, &age, 1);
}

bool ParticleInterpolation::write(uint32_t particle, const Drawn& drawn) {
    return m_writeWords(particle + kGlobalPosition, drawn.position.data(), kPositionWords) &&
           m_writeWords(particle + kDrawParams, drawn.params.data(), kDrawParamWords);
}

// A paint that draws the tick as it is: how each particle was drawn, for the next mid paint.
void ParticleInterpolation::record(uint32_t particle) {
    Seen seen{.tick = m_tick};
    uint32_t age = 0;
    if (!read(particle, seen.drawn, age)) {
        m_unreadable++;
        return;
    }
    seen.age = asFloat(age);
    m_seen[particle] = seen;
}

void ParticleInterpolation::blend(uint32_t particle) {
    if (!m_blended.insert(particle).second) {
        // A second draw of the emitter in this paint: already at its midpoint.
        return;
    }
    Drawn drawn;
    uint32_t ageWord = 0;
    if (!read(particle, drawn, ageWord)) {
        m_unreadable++;
        return;
    }
    auto seen = m_seen.find(particle);
    if (seen == m_seen.end() || seen->second.tick + 1 != m_tick) {
        m_particleFirstSeen++;
        return;
    }
    if (!(asFloat(ageWord) > seen->second.age)) {
        // The pool reused the slot for a new particle.
        m_particlesRenewed++;
        return;
    }
    const Drawn& from = seen->second.drawn;
    Drawn blended = drawn;
    std::span<const uint32_t> fromParams(from.params);
    std::span<const uint32_t> toParams(drawn.params);
    std::span<uint32_t> outParams(blended.params);
    if (!blendFloats({.from = from.position, .to = drawn.position}, blended.position) ||
        !blendFloats({.from = fromParams.subspan(kAxisWord, 3 + kScaleWords),
                      .to = toParams.subspan(kAxisWord, 3 + kScaleWords)},
                     outParams.subspan(kAxisWord, 3 + kScaleWords)) ||
        !blendFloats(
            {.from = fromParams.subspan(kAlphaWord, 1), .to = toParams.subspan(kAlphaWord, 1)},
            outParams.subspan(kAlphaWord, 1))) {
        m_unblendable++;
        return;
    }
    for (size_t word : {kPrmColorWord, kEnvColorWord}) {
        blended.params.at(word) =
            midpointColor({.from = fromParams.subspan(word, 1), .to = toParams.subspan(word, 1)});
    }
    // The angle wraps; its speed, the low half, is kept.
    auto angle = [](uint32_t word) {
        return static_cast<int16_t>(word >> 16);
    };
    auto mid = static_cast<uint16_t>(interp::midpointAngle(angle(from.params.at(kRotationWord)),
                                                           angle(drawn.params.at(kRotationWord))));
    blended.params.at(kRotationWord) =
        (uint32_t{mid} << 16) | (drawn.params.at(kRotationWord) & 0xffff);
    if (!write(particle, blended)) {
        // Part of it may have been written.
        write(particle, drawn);
        m_writeFailures++;
        return;
    }
    m_restores.push_back({.particle = particle, .drawn = drawn});
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
        if (write(restore.particle, restore.drawn)) {
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
