#pragma once

#include "wiiuport/control/ControlChannel.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/frame/ReplayScheduler.h"
#include "wiiuport/frame/SearchFeed.h"
#include "wiiuport/interp/TransformSearch.h"

namespace wiiuport {

// The one long-lived owner of everything first-party in a running process.
//
// The fork's hook registry holds a raw pointer, so whatever it points at has
// to outlive every frame. Keeping that here, in one object with one lifetime,
// is the alternative to each subsystem arranging its own global.
class Runtime {
  public:
    // Constructible directly so a test can drive one without touching the
    // process-wide instance. Not copyable: the hook registry holds a pointer
    // to whichever one was installed.
    Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    static Runtime& instance();

    // Idempotent: installing twice is a programming error at the call site,
    // not a reason to replace a registration while a frame is in flight.
    void installHooks();

    bool hooksInstalled() const {
        return m_hooksInstalled;
    }

    frame::RecordingObserver& recorder() {
        return m_recorder;
    }

    control::ControlChannel& control() {
        return m_control;
    }

    frame::FrameReplayer& replayer() {
        return m_replayer;
    }

    const interp::TransformSearch& transformSearch() const {
        return m_search;
    }

  private:
    frame::RecordingObserver m_recorder;
    frame::FrameReplayer m_replayer;
    frame::ReplayScheduler m_scheduler{m_replayer};
    interp::TransformSearch m_search;
    frame::SearchFeed m_searchFeed{m_search};
    control::ControlChannel m_control{m_recorder, m_replayer, m_search};
    bool m_hooksInstalled{false};
};

} // namespace wiiuport

// The fork's startup calls this, and names nothing first-party in doing so.
//
// Registration cannot be left to a static initialiser: this is a static
// library, and a linker drops an object file nothing references, taking the
// initialiser with it. The failure would be silent -- a build that records
// nothing and reports no error.
extern "C" void wiiuport_install_hooks(void);
