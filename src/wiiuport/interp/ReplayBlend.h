#pragma once

#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/interp/ObjectBlend.h"
#include "wiiuport/interp/TransformSubstitution.h"

namespace wiiuport::interp {

// The one filter over the runtime's replayed draws, and the order its two
// edits happen in. Objects first, because an object's blend rewrites its
// whole buffer, the view included; the camera is then written over it,
// blended as a pose rather than value by value.
class ReplayBlend final : public frame::AssemblyFilter {
  public:
    ReplayBlend(ObjectBlend& objects, TransformSubstitution& view)
        : m_objects(objects), m_view(view) {
    }

    bool onRuntimeAssembly(const LatteFrameHooks::UniformAssembly& assembly) override {
        bool movedObject = m_objects.apply(assembly);
        bool movedCamera = m_view.onRuntimeAssembly(assembly);
        return movedObject || movedCamera;
    }

  private:
    ObjectBlend& m_objects;
    TransformSubstitution& m_view;
};

} // namespace wiiuport::interp
