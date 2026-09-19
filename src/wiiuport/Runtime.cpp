#include "wiiuport/Runtime.h"

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

#include <lucent/config.h>
#include <lucent/log.h>

namespace wiiuport {
namespace {

// Deliberately never destroyed. The renderer can still be running at static
// destruction time, and the hook registry would be left pointing at a dead
// object.
Runtime g_runtime;

} // namespace

namespace {

bool submitToCommandProcessor(const void* data, uint32_t sizeInBytes) {
    return LatteFrameHooks::SubmitDisplayList(data, sizeInBytes);
}

} // namespace

Runtime::Runtime() : m_replayer(&submitToCommandProcessor) {
    m_recorder.addFrameEndListener(&m_scheduler);
    m_recorder.addFrameEndListener(&m_searchFeed);
}

Runtime& Runtime::instance() {
    return g_runtime;
}

void Runtime::installHooks() {
    if (m_hooksInstalled) {
        return;
    }
    LatteFrameHooks::SetObserver(&m_recorder);
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
