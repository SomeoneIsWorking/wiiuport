#include "wiiuport/Runtime.h"

#include "input/VPADInputHooks.h"

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

#include <lucent/config.h>
#include <lucent/log.h>

namespace wiiuport {
namespace {

bool requestFrameCapture(LatteFrameHooks::CaptureCallback callback) {
    return LatteFrameHooks::RequestFrameCapture(std::move(callback));
}

bool submitPresent(const LatteFrameHooks::PresentArguments& present) {
    return LatteFrameHooks::SubmitPresent(present);
}

bool submitScanBufferCopy(const LatteFrameHooks::PresentArguments& present) {
    return LatteFrameHooks::SubmitScanBufferCopy(present);
}

interp::ContinuousInterpolator::Clock::time_point steadyNow() {
    return interp::ContinuousInterpolator::Clock::now();
}

bool submitToCommandProcessor(const void* data, uint32_t sizeInBytes) {
    return LatteFrameHooks::SubmitDisplayList(data, sizeInBytes);
}

} // namespace

Runtime::Runtime()
    : m_replayer(&submitToCommandProcessor), m_presenter(&submitPresent, &submitScanBufferCopy),
      m_capture(&requestFrameCapture),
      m_guard(&LatteFrameHooks::GuardGuestState, &LatteFrameHooks::RestoreGuestState),
      m_continuous(m_viewTracker, m_substitution, m_objectBlend, m_replayer, m_presenter, m_guard,
                   m_scheduler, m_tickProbes, &steadyNow) {
    // Frame complete, before the guest's swap: everything that reads the
    // frame first, and the continuous interpolator last, because it needs the
    // view tracker and the object blend to have taken this frame in.
    m_recorder.addAssemblyRecordedListener(&m_objectBlend);
    m_recorder.addAssemblyRecordedListener(&m_vertexBlend);
    m_recorder.addDrawRecordedListener(&m_vertexBlend);
    m_recorder.addDisplayedListener(&m_pacing);
    m_recorder.addScanOutListener(&m_scanOut);
    m_recorder.addFrameEndListener(&m_searchFeed);
    m_recorder.addFrameEndListener(&m_shapeLog);
    m_recorder.addFrameEndListener(&m_viewTracker);
    m_recorder.addFrameEndListener(&m_snapshot);
    m_recorder.addFrameEndListener(&m_objectBlend);
    // After the object blend, whose frame count dates the vertices kept.
    m_recorder.addFrameEndListener(&m_vertexBlend);
    m_recorder.addFrameEndListener(&m_continuous);
    // Frame shown, after the guest's swap: the one-shots.
    m_recorder.addFrameShownListener(&m_scheduler);
    // After the scheduler, so the frame end that runs the replay has already
    // run it by the time the blend is taken down.
    m_recorder.addFrameShownListener(&m_interpolator);
    // Last, so every one-shot of the frame has run before the title is held.
    m_recorder.addFrameShownListener(&m_gate);
    m_recorder.addPresentListener(&m_presenter);
    // The blends only ever see the runtime's own replayed draws; the recorder
    // is what keeps the guest's frames out of their reach.
    m_recorder.setAssemblyFilter(&m_replayBlend);
    m_recorder.setVertexFilter(&m_vertexBlend);
}

Runtime& Runtime::instance() {
    std::call_once(s_created, [] {
        s_instance = new Runtime();
    });
    return *s_instance;
}

void Runtime::installHooks() {
    if (m_hooksInstalled) {
        return;
    }
    LatteFrameHooks::SetObserver(&m_recorder);
    VPADInputHooks::SetSource(&m_input);
    m_hooksInstalled = true;
    // Off unless a port is configured. The channel is how an agent asks a
    // running product what it is doing; a player never needs it.
    lucent::config::set_prefix("WIIUPORT_");
    long long port = lucent::config::number("CONTROL_PORT", 0);
    // On unless switched off: the product is the 60 Hz one, and the switch is
    // there to compare against the title's own rate.
    m_continuous.setEnabled(lucent::config::number("INTERPOLATION", 1) != 0);
    if (port > 0 && port <= 65535) {
        m_control.start(static_cast<uint16_t>(port));
    }
    lucent::info("runtime", "frame hooks installed; control channel {}; interpolation {}",
                 m_control.running() ? "up" : "off", m_continuous.enabled() ? "on" : "off");
}

} // namespace wiiuport

extern "C" void wiiuport_install_hooks(void) {
    wiiuport::Runtime::instance().installHooks();
}
