#include "wiiuport/Runtime.h"

#include "input/VPADInputHooks.h"

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

#include <lucent/config.h>
#include <lucent/log.h>

#include <thread>

namespace wiiuport {
namespace {

bool requestFrameCapture(LatteFrameHooks::CaptureCallback&& callback, int count) {
    return LatteFrameHooks::RequestFrameCapture(std::move(callback), count);
}

} // namespace

Runtime::Runtime() : m_capture(&requestFrameCapture) {
    // Frame complete, before the guest's swap: everything that reads the frame first.
    m_recorder.addDisplayedListener(&m_pacing);
    m_recorder.addScanOutListener(&m_scanOut);
    m_recorder.addFrameEndListener(&m_shapeLog);
    m_recorder.addFrameEndListener(&m_snapshot);
    // Last, so every one-shot of the frame has run before the title is held.
    m_recorder.addFrameShownListener(&m_gate);
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
    m_drawInterpolation.install();
    m_particleInterpolation.install();
    m_materialInterpolation.install();
    m_paint.install();
    m_logic.install();
    m_hooksInstalled = true;
    // Always open, on loopback: the channel is how an agent asks a running
    // product, the player's own session included, what it is doing.
    lucent::config::set_prefix("WIIUPORT_");
    std::string refusal;
    auto census = guest::CallerCensus::parse(lucent::config::text("CALLER_CENSUS"), refusal);
    if (census.has_value()) {
        m_callers.install(*census);
    } else {
        lucent::error("runtime", "WIIUPORT_CALLER_CENSUS: {}; no census", refusal);
    }
    long long port = lucent::config::number("CONTROL_PORT", control::ControlChannel::kDefaultPort);
    if (port > 0 && port <= 65535) {
        m_control.start(static_cast<uint16_t>(port));
    } else {
        lucent::error("runtime", "WIIUPORT_CONTROL_PORT {} is not a port; no control channel",
                      port);
    }
    lucent::info("runtime", "frame hooks installed; control channel {}",
                 m_control.running() ? "up" : "off");
}

} // namespace wiiuport

extern "C" void wiiuport_install_hooks(void) {
    wiiuport::Runtime::instance().installHooks();
}
