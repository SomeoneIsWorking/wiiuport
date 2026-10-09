#pragma once

#include <mutex>

#include "Cafe/HW/Espresso/GuestPatching.h"
#include "wiiuport/control/ControlChannel.h"
#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/frame/FrameGate.h"
#include "wiiuport/frame/FrameShapeLog.h"
#include "wiiuport/frame/PresentPacing.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/frame/RecordingSnapshot.h"
#include "wiiuport/guest/CallerCensus.h"
#include "wiiuport/input/InputDriver.h"
#include "wiiuport/title/DrawInterpolation.h"
#include "wiiuport/title/EnvironmentInterpolation.h"
#include "wiiuport/title/LogicGate.h"
#include "wiiuport/title/MaterialInterpolation.h"
#include "wiiuport/title/ParticleInterpolation.h"
#include "wiiuport/title/SeaInterpolation.h"
#include "wiiuport/title/WindWakerPaint.h"

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

    // The display paint mod. It is what presents more than once per title frame now, which is why
    // it replaced the continuous interpolator in the shell's presentation-mode decision.
    title::WindWakerPaint& paint() {
        return m_paint;
    }

    input::InputDriver& input() {
        return m_input;
    }

    frame::FrameCapture& capture() {
        return m_capture;
    }

    const frame::FrameShapeLog& shapeLog() const {
        return m_shapeLog;
    }

    frame::FrameGate& frameGate() {
        return m_gate;
    }

  private:
    inline static Runtime* s_instance{nullptr};
    inline static std::once_flag s_created;

    frame::RecordingObserver m_recorder;
    frame::FrameCapture m_capture;
    frame::FrameShapeLog m_shapeLog;
    guest::CallerCensus m_callers{&GuestCallProbes::Register, &GuestCallProbes::GuestBytes};
    title::LogicGate m_logic{{.registerProbe = &GuestCallProbes::Register,
                              .allocateCode = &GuestPatching::AllocateCode,
                              .allocateData = &GuestPatching::AllocateData,
                              .writeWord = &GuestPatching::WriteWord,
                              .readWord = &GuestPatching::ReadWord}};
    title::WindWakerPaint m_paint{&GuestCallProbes::Register,      &GuestPatching::AllocateCode,
                                  &GuestPatching::WriteWord,       &GuestPatching::ReadWord,
                                  &GuestPatching::SetSwapInterval, &GuestPatching::SwapInterval};
    title::ParticleInterpolation m_particleInterpolation{
        {.registerProbe = &GuestCallProbes::Register,
         .readWords = &GuestPatching::ReadWords,
         .writeWords = &GuestPatching::WriteDataWords}};
    title::EnvironmentInterpolation m_environmentInterpolation{
        {.readWords = &GuestPatching::ReadWords, .writeWords = &GuestPatching::WriteDataWords}};
    title::MaterialInterpolation m_materialInterpolation{
        {.registerProbe = &GuestCallProbes::Register,
         .readWords = &GuestPatching::ReadWords,
         .writeWords = &GuestPatching::WriteDataWords}};
    title::SeaInterpolation m_seaInterpolation{
        {.readWords = &GuestPatching::ReadWords, .writeWords = &GuestPatching::WriteDataWords}};
    title::DrawInterpolation m_drawInterpolation{
        {.registerProbe = &GuestCallProbes::Register,
         .readWords = &GuestPatching::ReadWords,
         .writeWords = &GuestPatching::WriteDataWords,
         .gated =
             [this] {
                 return m_logic.enabled();
             },
         .midPaint = {&m_particleInterpolation, &m_environmentInterpolation},
         .drawPhase = {&m_seaInterpolation, &m_materialInterpolation}}};
    frame::RecordingSnapshot m_snapshot;
    frame::PresentPacing m_pacing{&frame::PresentPacing::Clock::now};
    frame::PresentPacing m_scanOut{&frame::PresentPacing::Clock::now};
    input::InputDriver m_input;
    frame::FrameGate m_gate;
    control::ControlChannel m_control{control::ControlChannel::Sources{
        .recorder = m_recorder,
        .input = m_input,
        .capture = m_capture,
        .shapeLog = m_shapeLog,
        .callers = m_callers,
        .paint = m_paint,
        .drawInterpolation = m_drawInterpolation,
        .particleInterpolation = m_particleInterpolation,
        .seaInterpolation = m_seaInterpolation,
        .materialInterpolation = m_materialInterpolation,
        .environmentInterpolation = m_environmentInterpolation,
        .logic = m_logic,
        .guestBytes = &GuestCallProbes::GuestBytes,
        .snapshot = m_snapshot,
        .pacing = m_pacing,
        .scanOut = m_scanOut,
        .vertexChanges = m_recorder.vertexChanges(),
        .gate = m_gate,
    }};
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
