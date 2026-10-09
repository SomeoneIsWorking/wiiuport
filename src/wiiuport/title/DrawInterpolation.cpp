#include "wiiuport/title/DrawInterpolation.h"

#include "wiiuport/guest/ProbeInstallation.h"
#include "wiiuport/interp/Blendable.h"
#include "wiiuport/title/JsonBody.h"

#include <bit>
#include <utility>

namespace wiiuport::title {

namespace {

float asFloat(uint32_t word) {
    return std::bit_cast<float>(word);
}

uint32_t asWord(float value) {
    return std::bit_cast<uint32_t>(value);
}

// The midpoint of two floats held as words, or nothing when either is not a number.
std::optional<uint32_t> midpointWord(uint32_t from, uint32_t to) {
    std::array<float, 1> out{};
    std::array<float, 1> fromValue{asFloat(from)};
    std::array<float, 1> toValue{asFloat(to)};
    if (!interp::midpoint(fromValue, toValue, out)) {
        return std::nullopt;
    }
    return asWord(out[0]);
}

// Two s16 angles in one big-endian word, high half first.
uint32_t midpointAngles(uint32_t from, uint32_t to) {
    auto high = [](uint32_t word) {
        return static_cast<int16_t>(word >> 16);
    };
    auto low = [](uint32_t word) {
        return static_cast<int16_t>(word & 0xffff);
    };
    auto highMid = static_cast<uint16_t>(interp::midpointAngle(high(from), high(to)));
    auto lowMid = static_cast<uint16_t>(interp::midpointAngle(low(from), low(to)));
    return (uint32_t{highMid} << 16) | lowMid;
}

} // namespace

void DrawInterpolation::Entry::OnInstall(GuestCallProbes::Installation value) {
    std::scoped_lock lock(m_owner.m_mutex);
    installation = value;
}

void DrawInterpolation::Entry::OnCall(std::span<const uint32_t, 32> gpr,
                                      uint32_t /*returnAddress*/) {
    switch (m_event) {
    case Event::Management:
        m_owner.onManagement();
        break;
    case Event::CameraDraw:
        m_owner.onCameraDraw(gpr[kProcessRegister]);
        break;
    case Event::ActorDraw:
        m_owner.onActorDraw(gpr[kProcessRegister]);
        break;
    case Event::AfterDraw:
        m_owner.onAfterDraw();
        break;
    }
}

DrawInterpolation::DrawInterpolation(Seams seams)
    : m_register(seams.registerProbe), m_readWords(std::move(seams.readWords)),
      m_writeWords(std::move(seams.writeWords)), m_gated(std::move(seams.gated)) {
}

void DrawInterpolation::install() {
    m_register(kManagement, kManagementFirst, m_management, true, 0);
    m_register(kCameraDraw, kCameraDrawFirst, m_cameraDraw, true, 0);
    m_register(kActorDraw, kActorDrawFirst, m_actorDraw, true, 0);
    m_register(kAfterDraw, kAfterDrawFirst, m_afterDraw, true, 0);
}

void DrawInterpolation::setEnabled(bool enabled) {
    std::scoped_lock lock(m_mutex);
    m_enabled = enabled;
}

void DrawInterpolation::onManagement() {
    bool gated = m_gated();
    std::scoped_lock lock(m_mutex);
    m_inTick = gated && m_enabled;
    if (m_inTick) {
        m_tick++;
        m_ticks++;
    }
}

void DrawInterpolation::onCameraDraw(uint32_t camera) {
    std::scoped_lock lock(m_mutex);
    if (!m_inTick) {
        return;
    }
    std::array<uint32_t, kCameraWords> words{};
    if (!m_readWords(camera + kCameraInputs, words.data(), kCameraWords)) {
        m_unreadable++;
        return;
    }
    if (blendCamera(camera, words)) {
        m_cameraBlends++;
    }
}

bool DrawInterpolation::blendCamera(uint32_t camera,
                                    const std::array<uint32_t, kCameraWords>& words) {
    Seen<kCameraWords>& seen = m_cameras[camera];
    bool continues = seen.tick != 0 && seen.tick + 1 == m_tick;
    std::array<uint32_t, kCameraWords> previous = seen.words;
    seen = {.tick = m_tick, .words = words};
    if (!continues) {
        m_cameraFirstSeen++;
        return false;
    }
    std::array<uint32_t, kCameraWords> blended = words;
    for (size_t word = 0; word < kBankWord; ++word) {
        if (word == kAspectWord) {
            continue;
        }
        auto mid = midpointWord(previous.at(word), words.at(word));
        if (!mid) {
            m_unblendable++;
            return false;
        }
        blended.at(word) = *mid;
    }
    // The bank is the high half; the low half is padding and kept.
    blended.at(kBankWord) =
        (midpointAngles(previous.at(kBankWord), words.at(kBankWord)) & 0xffff0000) |
        (words.at(kBankWord) & 0xffff);
    write(camera + kCameraInputs, {.blended = blended, .original = words});
    return true;
}

void DrawInterpolation::onActorDraw(uint32_t actor) {
    std::scoped_lock lock(m_mutex);
    if (!m_inTick) {
        return;
    }
    m_actorDraws++;
    uint32_t condition = 0;
    std::array<uint32_t, kPlacementWords> words{};
    if (!m_readWords(actor + kCondition, &condition, 1) ||
        !m_readWords(actor + kPlacement, words.data(), kPlacementWords)) {
        m_unreadable++;
        return;
    }
    if ((condition & kNotExecuted) != 0) {
        // Not executed this tick: old is from whenever it last ran.
        m_actorsNotExecuted++;
        m_shapes[actor] = {.tick = m_tick,
                           .words = {words.at(kShapeAngleWord), words.at(kShapeAngleWord + 1)}};
        return;
    }
    if (blendActor(actor, words)) {
        m_actorBlends++;
    }
}

bool DrawInterpolation::blendActor(uint32_t actor,
                                   const std::array<uint32_t, kPlacementWords>& words) {
    std::array<uint32_t, kPosWords> position{};
    for (size_t axis = 0; axis < kPosWords; ++axis) {
        auto mid = midpointWord(words.at(kOldPosWord + axis), words.at(kCurrentPosWord + axis));
        if (!mid) {
            m_unblendable++;
            return false;
        }
        position.at(axis) = *mid;
    }
    std::array<uint32_t, 2> shape{words.at(kShapeAngleWord), words.at(kShapeAngleWord + 1)};
    Seen<2>& seen = m_shapes[actor];
    bool continues = seen.tick != 0 && seen.tick + 1 == m_tick;
    std::array<uint32_t, 2> previousShape = seen.words;
    seen = {.tick = m_tick, .words = shape};
    std::array<uint32_t, kPlacementWords> blended = words;
    for (size_t axis = 0; axis < kPosWords; ++axis) {
        blended.at(kCurrentPosWord + axis) = position.at(axis);
    }
    if (continues) {
        blended.at(kShapeAngleWord) = midpointAngles(previousShape[0], shape[0]);
        // z is the high half; the low half is padding and kept.
        blended.at(kShapeAngleWord + 1) =
            (midpointAngles(previousShape[1], shape[1]) & 0xffff0000) | (shape[1] & 0xffff);
    }
    // Current position through shape angle, the only words that change.
    size_t first = kCurrentPosWord;
    size_t count = kShapeAngleWord + 2 - kCurrentPosWord;
    write(actor + kPlacement + static_cast<uint32_t>(4 * first),
          {.blended = std::span<const uint32_t>(blended).subspan(first, count),
           .original = std::span<const uint32_t>(words).subspan(first, count)});
    return true;
}

void DrawInterpolation::write(uint32_t address, Change change) {
    if (!m_writeWords(address, change.blended.data(),
                      static_cast<uint32_t>(change.blended.size()))) {
        m_writeFailures++;
        return;
    }
    m_restores.push_back(
        {.address = address, .words = {change.original.begin(), change.original.end()}});
}

void DrawInterpolation::onAfterDraw() {
    std::scoped_lock lock(m_mutex);
    if (!m_inTick) {
        return;
    }
    m_inTick = false;
    for (const Restore& restore : m_restores) {
        if (m_writeWords(restore.address, restore.words.data(),
                         static_cast<uint32_t>(restore.words.size()))) {
            m_restored++;
        } else {
            m_writeFailures++;
        }
    }
    m_restores.clear();
    std::erase_if(m_shapes, [this](const auto& entry) {
        return entry.second.tick < m_tick;
    });
    std::erase_if(m_cameras, [this](const auto& entry) {
        return entry.second.tick < m_tick;
    });
}

std::string DrawInterpolation::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("management", std::string(guest::installationName(m_management.installation)));
    body.string("cameraDraw", std::string(guest::installationName(m_cameraDraw.installation)));
    body.string("actorDraw", std::string(guest::installationName(m_actorDraw.installation)));
    body.string("afterDraw", std::string(guest::installationName(m_afterDraw.installation)));
    body.raw("enabled", m_enabled ? "true" : "false");
    body.number("ticks", m_ticks);
    body.number("cameraBlends", m_cameraBlends);
    body.number("cameraFirstSeen", m_cameraFirstSeen);
    body.number("actorDraws", m_actorDraws);
    body.number("actorBlends", m_actorBlends);
    body.number("actorsNotExecuted", m_actorsNotExecuted);
    body.number("unblendable", m_unblendable);
    body.number("unreadable", m_unreadable);
    body.number("writeFailures", m_writeFailures);
    body.number("restored", m_restored);
    return body.finish();
}

} // namespace wiiuport::title
