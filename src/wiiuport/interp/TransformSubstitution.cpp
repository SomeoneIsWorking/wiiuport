#include "wiiuport/interp/TransformSubstitution.h"

namespace wiiuport::interp {

bool TransformSubstitution::armOnce(const std::vector<ViewSlot>& slots, float t) {
    if (slots.empty()) {
        return false;
    }
    m_blended.clear();
    m_offered.clear();
    for (const ViewSlot& slot : slots) {
        m_blended[slot.shader] =
            BlendedSlot{slot.floatOffset, Transform3x4::blendView(slot.before, slot.after, t)};
    }
    m_blendPoint = t;
    m_armed = true;
    return true;
}

std::vector<TransformSubstitution::OfferedShader> TransformSubstitution::armedSlots() const {
    std::vector<OfferedShader> slots;
    slots.reserve(m_blended.size());
    for (const auto& [shader, blended] : m_blended) {
        slots.push_back(OfferedShader{shader, blended.floatOffset, 0, false});
    }
    return slots;
}

void TransformSubstitution::noteOffered(const LatteFrameHooks::UniformAssembly& assembly) {
    ShaderKey key{assembly.shaderBaseHash, assembly.shaderAuxHash, assembly.stageIndex};
    for (OfferedShader& offered : m_offered) {
        if (offered.shader == key) {
            ++offered.times;
            return;
        }
    }
    if (m_offered.size() >= kMaxOfferedShaders) {
        return;
    }
    m_offered.push_back(OfferedShader{key, assembly.sizeInBytes / uint32_t{sizeof(float)}, 1,
                                      m_blended.find(key) != m_blended.end()});
}

bool TransformSubstitution::onRuntimeAssembly(const LatteFrameHooks::UniformAssembly& assembly) {
    ++m_assembliesOffered;
    noteOffered(assembly);
    if (!m_armed) {
        ++m_assembliesUnarmed;
        return false;
    }
    auto found = m_blended.find(
        ShaderKey{assembly.shaderBaseHash, assembly.shaderAuxHash, assembly.stageIndex});
    if (found == m_blended.end()) {
        // Most draws are not the ones carrying the view. Counting them is
        // what makes "the view shader was never drawn" visible.
        ++m_assembliesUnknownShader;
        return false;
    }
    const BlendedSlot& slot = found->second;
    size_t floats = assembly.sizeInBytes / sizeof(float);
    if (slot.floatOffset + static_cast<size_t>(Transform3x4::kFloats) > floats) {
        // The buffer this draw assembled is shorter than the one the view was
        // found in. Writing anyway would run past it.
        ++m_assembliesTooShort;
        return false;
    }
    slot.value.writeRowMajor(assembly.data + slot.floatOffset);
    ++m_assembliesSubstituted;
    return true;
}

} // namespace wiiuport::interp
