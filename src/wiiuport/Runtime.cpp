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

bool submitToCommandProcessor(const void* data, uint32_t sizeInBytes) {
    return LatteFrameHooks::SubmitDisplayList(data, sizeInBytes);
}

} // namespace

Runtime::Runtime()
    : m_replayer(&submitToCommandProcessor), m_presenter(&submitPresent),
      m_capture(&requestFrameCapture) {
    m_recorder.addFrameEndListener(&m_scheduler);
    // After the scheduler, so the frame end that runs the replay has already
    // run it by the time the blend is taken down.
    m_recorder.addFrameEndListener(&m_interpolator);
    m_recorder.addFrameEndListener(&m_searchFeed);
    m_recorder.addFrameEndListener(&m_shapeLog);
    m_recorder.addPresentListener(&m_presenter);
    // The substitution only ever sees the runtime's own replayed draws; the
    // recorder is what keeps the guest's frames out of its reach.
    m_recorder.setAssemblyFilter(&m_substitution);
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
    if (port > 0 && port <= 65535) {
        m_control.start(static_cast<uint16_t>(port));
    }
    lucent::info("runtime", "frame hooks installed; control channel {}",
                 m_control.running() ? "up" : "off");
}

} // namespace wiiuport

extern "C" void wiiuport_install_hooks(void) {
    wiiuport::Runtime::instance().installHooks();
}
