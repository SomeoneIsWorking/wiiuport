#include "wiiuport/title/DrawInterpolation.h"

#include "wiiuport/guest/ProbeInstallation.h"
#include "wiiuport/interp/Affine.h"
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
    case Event::ModelView:
        m_owner.onModelView(gpr[kProcessRegister]);
        break;
    case Event::BeforeDraw:
        m_owner.onBeforeDraw();
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
    m_register(kModelView, kModelViewFirst, m_modelView, true, 0);
    m_register(kBeforeDraw, kBeforeDrawFirst, m_beforeDraw, true, 0);
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

void DrawInterpolation::onModelView(uint32_t model) {
    std::scoped_lock lock(m_mutex);
    if (!m_inDraw || !m_enabled || !m_viewedInDraw.insert(model).second) {
        return;
    }
    m_modelViews++;
    std::optional<ModelPose> pose = readModel(model);
    if (!pose) {
        m_unreadable++;
        return;
    }
    if (!m_inTick) {
        recordModel(model, std::move(*pose));
        return;
    }
    m_tickBases[model] = {.tick = m_tick, .words = pose->base};
    if (blendModel(model, *pose)) {
        m_modelBlends++;
    }
}

std::optional<DrawInterpolation::ModelPose> DrawInterpolation::readModel(uint32_t model) {
    ModelPose pose;
    uint32_t skeleton = 0;
    uint32_t joints = 0;
    if (!m_readWords(model + kModelBase, pose.base.data(), kMatrixWords) ||
        !m_readWords(model + kModelSkeleton, &skeleton, 1) || skeleton == 0 ||
        !m_readWords(skeleton + kSkeletonWorld, &pose.world, 1) || pose.world == 0 ||
        !m_readWords(skeleton + kSkeletonJoints, &joints, 1)) {
        return std::nullopt;
    }
    joints >>= 16;
    if (joints == 0 || joints > kMaxJoints) {
        return std::nullopt;
    }
    pose.joints.resize(size_t{joints} * kMatrixWords);
    if (!m_readWords(pose.world, pose.joints.data(), static_cast<uint32_t>(pose.joints.size()))) {
        return std::nullopt;
    }
    return pose;
}

// The skipped call's draw phase: the tick's own pose, with no input at a midpoint.
void DrawInterpolation::recordModel(uint32_t model, ModelPose pose) {
    SeenModel& seen = m_models[model];
    auto tickBase = m_tickBases.find(model);
    if (tickBase != m_tickBases.end() && tickBase->second.tick == m_tick) {
        if (tickBase->second.words != pose.base) {
            seen.baseFromDraw = true;
        } else if (seen.tick + 1 == m_tick && seen.pose.base != pose.base) {
            // Moved, and the tick's draw had the same base: it is not from the draw's inputs.
            seen.baseFromDraw = false;
        }
    }
    seen.tick = m_tick;
    seen.pose = std::move(pose);
}

namespace {

std::optional<interp::Affine> affineAt(std::span<const uint32_t> words) {
    interp::Affine out;
    for (size_t i = 0; i < out.m.size(); ++i) {
        float value = asFloat(words[i]);
        if (!interp::isNumber(value)) {
            return std::nullopt;
        }
        out.m.at(i) = value;
    }
    return out;
}

bool writeAffine(const interp::Affine& affine, std::span<uint32_t> words) {
    for (size_t i = 0; i < affine.m.size(); ++i) {
        auto value = static_cast<float>(affine.m.at(i));
        if (!std::isfinite(value)) {
            return false;
        }
        words[i] = asWord(value);
    }
    return true;
}

} // namespace

// Each joint at the midpoint of its two ticks' model-space pose, placed at the midpoint base.
bool DrawInterpolation::blendModel(uint32_t model, const ModelPose& pose) {
    auto seen = m_models.find(model);
    if (seen == m_models.end() || seen->second.tick + 1 != m_tick ||
        seen->second.pose.joints.size() != pose.joints.size()) {
        m_modelFirstSeen++;
        return false;
    }
    const ModelPose& previous = seen->second.pose;
    std::optional<interp::Affine> previousBase = affineAt(previous.base);
    std::optional<interp::Affine> base = affineAt(pose.base);
    std::optional<interp::Affine> previousInverse =
        previousBase ? interp::inverse(*previousBase) : std::nullopt;
    std::optional<interp::Affine> inverse = base ? interp::inverse(*base) : std::nullopt;
    if (!previousInverse || !inverse) {
        m_unblendable++;
        return false;
    }
    if (seen->second.baseFromDraw) {
        m_modelBasesFromDraw++;
    }
    interp::Affine placed =
        seen->second.baseFromDraw ? *base : interp::midpoint(*previousBase, *base);
    std::vector<uint32_t> blended(pose.joints.size());
    for (size_t joint = 0; joint < pose.joints.size(); joint += kMatrixWords) {
        auto from = affineAt(std::span(previous.joints).subspan(joint, kMatrixWords));
        auto to = affineAt(std::span(pose.joints).subspan(joint, kMatrixWords));
        if (!from || !to) {
            m_unblendable++;
            return false;
        }
        interp::Affine local = interp::midpoint(*previousInverse * *from, *inverse * *to);
        if (!writeAffine(placed * local, std::span(blended).subspan(joint, kMatrixWords))) {
            m_unblendable++;
            return false;
        }
    }
    write(pose.world, {.blended = blended, .original = pose.joints});
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

void DrawInterpolation::onBeforeDraw() {
    std::scoped_lock lock(m_mutex);
    m_inDraw = true;
    m_viewedInDraw.clear();
}

void DrawInterpolation::onAfterDraw() {
    std::scoped_lock lock(m_mutex);
    m_inDraw = false;
    if (!m_inTick) {
        std::erase_if(m_models, [this](const auto& entry) {
            return entry.second.tick < m_tick;
        });
        std::erase_if(m_tickBases, [this](const auto& entry) {
            return entry.second.tick < m_tick;
        });
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
    body.string("modelView", std::string(guest::installationName(m_modelView.installation)));
    body.string("beforeDraw", std::string(guest::installationName(m_beforeDraw.installation)));
    body.string("afterDraw", std::string(guest::installationName(m_afterDraw.installation)));
    body.raw("enabled", m_enabled ? "true" : "false");
    body.number("ticks", m_ticks);
    body.number("cameraBlends", m_cameraBlends);
    body.number("cameraFirstSeen", m_cameraFirstSeen);
    body.number("actorDraws", m_actorDraws);
    body.number("actorBlends", m_actorBlends);
    body.number("actorsNotExecuted", m_actorsNotExecuted);
    body.number("modelViews", m_modelViews);
    body.number("modelBlends", m_modelBlends);
    body.number("modelFirstSeen", m_modelFirstSeen);
    body.number("modelBasesFromDraw", m_modelBasesFromDraw);
    body.number("unblendable", m_unblendable);
    body.number("unreadable", m_unreadable);
    body.number("writeFailures", m_writeFailures);
    body.number("restored", m_restored);
    return body.finish();
}

} // namespace wiiuport::title
