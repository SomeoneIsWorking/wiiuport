#include "wiiuport/title/MaterialInterpolation.h"

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

float highHalf(uint32_t word) {
    return static_cast<float>(static_cast<int16_t>(word >> 16));
}

float lowHalf(uint32_t word) {
    return static_cast<float>(static_cast<int16_t>(word & 0xffff));
}

// A step no longer than the rate, allowing for rounding.
bool withinStep(float delta, float rate) {
    return std::fabs(delta) <= (std::fabs(rate) * 1.001f) + 1.0e-4f;
}

} // namespace

void MaterialInterpolation::Entry::OnInstall(GuestCallProbes::Installation value) {
    std::scoped_lock lock(m_owner.m_mutex);
    installation = value;
}

void MaterialInterpolation::Entry::OnCall(std::span<const uint32_t, 32> gpr,
                                          uint32_t /*returnAddress*/) {
    m_owner.onEntry(m_point.animation, gpr[kAnimationRegister]);
}

void MaterialInterpolation::Play::OnInstall(GuestCallProbes::Installation value) {
    std::scoped_lock lock(m_owner.m_mutex);
    installation = value;
}

void MaterialInterpolation::Play::OnCall(std::span<const uint32_t, 32> gpr,
                                         uint32_t /*returnAddress*/) {
    m_owner.onPlay(gpr[kAnimationRegister]);
}

MaterialInterpolation::MaterialInterpolation(Seams seams)
    : m_register(seams.registerProbe), m_readWords(std::move(seams.readWords)),
      m_writeWords(std::move(seams.writeWords)) {
}

void MaterialInterpolation::install() {
    for (size_t i = 0; i < m_entries.size(); ++i) {
        m_register(kEntryPoints.at(i).address, kEntryFirst, m_entries.at(i), true, 0);
    }
    m_register(kPlay, kPlayFirst, m_play, true, 0);
}

void MaterialInterpolation::onDrawPhaseBegin(uint64_t tick) {
    std::scoped_lock lock(m_mutex);
    m_inDraw = true;
    m_tick = tick;
    m_actor = 0;
}

void MaterialInterpolation::onActorDraw(uint32_t actor) {
    std::scoped_lock lock(m_mutex);
    m_actor = actor;
    auto owned = m_owned.find(actor);
    if (owned == m_owned.end()) {
        return;
    }
    std::vector<Animation> animations = std::move(owned->second.animations);
    owned->second = {.tick = m_tick};
    for (const Animation& animation : animations) {
        blend(animation);
    }
}

// The entry its draw makes, after the frame was blended or not: owned for the next tick.
void MaterialInterpolation::onEntry(uint32_t animationOffset, uint32_t animation) {
    std::scoped_lock lock(m_mutex);
    if (!m_inDraw || m_actor == 0) {
        return;
    }
    m_entriesSeen++;
    uint32_t j3dAnimation = 0;
    if (!m_readWords(animation + animationOffset, &j3dAnimation, 1)) {
        m_unreadable++;
        return;
    }
    m_entered[animation] = m_tick;
    Owned& owned = m_owned[m_actor];
    owned.tick = m_tick;
    for (const Animation& known : owned.animations) {
        if (known.address == animation) {
            return;
        }
    }
    owned.animations.push_back(
        {.address = animation, .animationOffset = animationOffset, .j3dAnimation = j3dAnimation});
}

// A play in the gated draw phase of an animation a draw entered: half a step back, so the play
// lands between this tick's frame and the next.
void MaterialInterpolation::onPlay(uint32_t controller) {
    std::scoped_lock lock(m_mutex);
    if (!m_inDraw || !m_entered.contains(controller)) {
        return;
    }
    std::array<uint32_t, kFrameWord + 1> words{};
    if (!m_readWords(controller, words.data(), static_cast<uint32_t>(words.size()))) {
        m_unreadable++;
        return;
    }
    m_playedInDraw[controller] = m_tick;
    float frame = asFloat(words.at(kFrameWord));
    float rate = asFloat(words.at(kRateWord));
    if (!interp::isNumber(frame) || !interp::isNumber(rate)) {
        return;
    }
    uint32_t back = std::bit_cast<uint32_t>(frame - (rate / 2.0f));
    if (!m_writeWords(controller + kFrame, &back, 1)) {
        m_writeFailures++;
        return;
    }
    m_restores.push_back({.address = controller + kFrame, .frame = words.at(kFrameWord)});
    m_drawPlays++;
}

void MaterialInterpolation::blend(const Animation& animation) {
    if (m_playedInDraw.contains(animation.address)) {
        return;
    }
    uint32_t j3dAnimation = 0;
    std::array<uint32_t, kControllerWords> controller{};
    if (!m_readWords(animation.address + animation.animationOffset, &j3dAnimation, 1) ||
        !m_readWords(animation.address, controller.data(), kControllerWords)) {
        m_unreadable++;
        return;
    }
    if (j3dAnimation != animation.j3dAnimation) {
        m_replaced++;
        return;
    }
    float frame = asFloat(controller.at(kFrameWord));
    float rate = asFloat(controller.at(kRateWord));
    Seen& seen = m_seen[animation.address];
    Seen previous = std::exchange(seen, {.tick = m_tick, .frame = frame});
    if (previous.tick + 1 != m_tick || !interp::isNumber(frame) || !interp::isNumber(rate) ||
        !interp::isNumber(previous.frame)) {
        m_firstSeen++;
        return;
    }
    float delta = frame - previous.frame;
    float mid = previous.frame + (delta / 2.0f);
    if (!withinStep(delta, rate)) {
        // A looping controller wraps from its end back to its loop frame.
        float end = lowHalf(controller.at(kStartEndWord));
        float loop = highHalf(controller.at(kLoopAttributeWord));
        float span = end - loop;
        uint32_t attribute = (controller.at(kLoopAttributeWord) >> 8) & 0xff;
        float wrapped = delta + (rate > 0.0f ? span : -span);
        if (attribute != kLoopAttribute || span <= 0.0f || !withinStep(wrapped, rate)) {
            m_jumps++;
            return;
        }
        mid = previous.frame + (wrapped / 2.0f);
        if (mid >= end) {
            mid -= span;
        } else if (mid < loop) {
            mid += span;
        }
        m_wraps++;
    }
    if (mid == frame) {
        return;
    }
    uint32_t word = std::bit_cast<uint32_t>(mid);
    if (!m_writeWords(animation.address + kFrame, &word, 1)) {
        m_writeFailures++;
        return;
    }
    m_restores.push_back(
        {.address = animation.address + kFrame, .frame = controller.at(kFrameWord)});
    m_blends++;
}

void MaterialInterpolation::onDrawPhaseEnd() {
    std::scoped_lock lock(m_mutex);
    // Last first: a play after a blend holds the blended frame.
    for (auto restore = m_restores.rbegin(); restore != m_restores.rend(); ++restore) {
        if (m_writeWords(restore->address, &restore->frame, 1)) {
            m_restored++;
        } else {
            m_writeFailures++;
        }
    }
    m_restores.clear();
    m_inDraw = false;
    m_actor = 0;
    std::erase_if(m_owned, [this](const auto& entry) {
        return entry.second.tick < m_tick;
    });
    std::erase_if(m_seen, [this](const auto& entry) {
        return entry.second.tick < m_tick;
    });
    std::erase_if(m_entered, [this](const auto& entry) {
        return entry.second < m_tick;
    });
    std::erase_if(m_playedInDraw, [this](const auto& entry) {
        return entry.second < m_tick;
    });
}

std::string MaterialInterpolation::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.number("entriesSeen", m_entriesSeen);
    body.number("blends", m_blends);
    body.number("wraps", m_wraps);
    body.number("firstSeen", m_firstSeen);
    body.number("jumps", m_jumps);
    body.number("replaced", m_replaced);
    body.number("drawPlays", m_drawPlays);
    body.number("unreadable", m_unreadable);
    body.number("writeFailures", m_writeFailures);
    body.number("restored", m_restored);
    return body.finish();
}

} // namespace wiiuport::title
