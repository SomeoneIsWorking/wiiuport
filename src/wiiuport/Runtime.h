#pragma once

#include <mutex>

#include "wiiuport/control/ControlChannel.h"
#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/frame/FramePresenter.h"
#include "wiiuport/frame/FrameShapeLog.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/frame/ReplayScheduler.h"
#include "wiiuport/frame/SearchFeed.h"
#include "wiiuport/input/InputDriver.h"
#include "wiiuport/interp/FrameInterpolator.h"
#include "wiiuport/interp/TransformSearch.h"
#include "wiiuport/interp/TransformSubstitution.h"

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

    // The process-wide instance. Never destroyed, and leaked on purpose: a
    // static destroyed at exit leaves the hook registry pointing at a dead
    // object and the control channel's server torn down underneath its own
    // thread, which ends a failed run in std::terminate rather than with the
    // exit code that says what went wrong.
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

    input::InputDriver& input() {
        return m_input;
    }

    frame::FrameCapture& capture() {
        return m_capture;
    }

    frame::FramePresenter& presenter() {
        return m_presenter;
    }

    const frame::FrameShapeLog& shapeLog() const {
        return m_shapeLog;
    }

    frame::ReplayScheduler& scheduler() {
        return m_scheduler;
    }

    interp::FrameInterpolator& interpolator() {
        return m_interpolator;
    }

  private:
    inline static Runtime* s_instance{nullptr};
    inline static std::once_flag s_created;

    frame::RecordingObserver m_recorder;
    frame::FrameReplayer m_replayer;
    frame::FramePresenter m_presenter;
    frame::FrameCapture m_capture;
    frame::ReplayScheduler m_scheduler{m_replayer, m_presenter, m_capture};
    interp::TransformSearch m_search;
    frame::SearchFeed m_searchFeed{m_search};
    frame::FrameShapeLog m_shapeLog;
    interp::TransformSubstitution m_substitution;
    interp::FrameInterpolator m_interpolator{m_search, m_substitution, m_scheduler};
    input::InputDriver m_input;
    control::ControlChannel m_control{m_recorder,  m_replayer,     m_search,
                                      m_input,     m_capture,      m_presenter,
                                      m_scheduler, m_interpolator, m_shapeLog};
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
