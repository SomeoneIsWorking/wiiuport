#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/interp/TransformSearch.h"

#include <cstdint>
#include <map>
#include <vector>

namespace wiiuport::interp {

// Writes a blended view transform into the runtime's own replayed draws.
//
// This is the whole of interpolation at the point where it happens: the
// renderer has assembled the uniform buffer for one draw, and this is the
// last moment before it is uploaded. The geometry replayed is the frame the
// title drew; only where the camera stands is different, so the extra frame
// shows the same world from between two of the title's own positions.
//
// It edits the runtime's replay and never the guest's frame, which the
// recorder enforces by offering it only the runtime's assemblies.
//
// Armed one frame at a time. A substitution left running would keep writing
// last frame's blend over draws that have moved on.
class TransformSubstitution final : public frame::AssemblyFilter {
  public:
    // Blends each slot's two endpoints at `t` and holds the result, so the
    // draw path copies twelve floats and does no arithmetic. False when there
    // is nothing to substitute, which is a refusal and not an arming that
    // silently writes nothing.
    bool armOnce(const std::vector<ViewSlot>& slots, float t);

    bool isArmed() const {
        return m_armed;
    }

    void disarm() {
        m_armed = false;
    }

    bool onRuntimeAssembly(const LatteFrameHooks::UniformAssembly& assembly) override;

    // What the blend was armed with, so a frame that looks unchanged can be
    // told from one where nothing was armed.
    float blendPoint() const {
        return m_blendPoint;
    }

    size_t slotCount() const {
        return m_blended.size();
    }

    // Denominators. "Substituted nothing" has three causes and they are
    // counted apart: the draw was not one of the shaders that carries the
    // view, its buffer was shorter than the offset the view sits at, or
    // nothing was armed at all.
    uint64_t assembliesOffered() const {
        return m_assembliesOffered;
    }

    uint64_t assembliesSubstituted() const {
        return m_assembliesSubstituted;
    }

    uint64_t assembliesUnarmed() const {
        return m_assembliesUnarmed;
    }

    uint64_t assembliesUnknownShader() const {
        return m_assembliesUnknownShader;
    }

    uint64_t assembliesTooShort() const {
        return m_assembliesTooShort;
    }

    // One offered draw, so "none of them carried the view" can be read as two
    // key sets side by side rather than as a number. Without it, a shader
    // identity that does not survive the replay looks exactly like a frame
    // that stopped drawing the world.
    struct OfferedShader {
        ShaderKey shader;
        uint32_t floats{0};
        uint64_t times{0};
        bool substituted{false};
    };

    // The armed slots, and the distinct shaders offered since arming, both
    // capped. Empty offered list with a non-zero offered count would be the
    // cap lying; it is bounded by distinct shader, not by draw.
    std::vector<OfferedShader> armedSlots() const;

    const std::vector<OfferedShader>& offeredShaders() const {
        return m_offered;
    }

  private:
    void noteOffered(const LatteFrameHooks::UniformAssembly& assembly);

    struct BlendedSlot {
        uint32_t floatOffset{0};
        Transform3x4 value;
    };

    // Bounds what a frame of draws can leave behind. High enough that a
    // frame's whole shader set fits; a frame with more is reported by the
    // offered counter, which is never capped.
    static constexpr size_t kMaxOfferedShaders = 64;

    std::map<ShaderKey, BlendedSlot> m_blended;
    std::vector<OfferedShader> m_offered;
    float m_blendPoint{0.0f};
    bool m_armed{false};
    uint64_t m_assembliesOffered{0};
    uint64_t m_assembliesSubstituted{0};
    uint64_t m_assembliesUnarmed{0};
    uint64_t m_assembliesUnknownShader{0};
    uint64_t m_assembliesTooShort{0};
};

} // namespace wiiuport::interp
