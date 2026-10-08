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

title::CommandPosition Runtime::gx2WritePosition() {
    const LatteFrameHooks::CommandWritePosition at = LatteFrameHooks::GetCommandWritePosition();
    return {at.bufferStart, at.bufferEnd, at.write};
}

Runtime::Runtime() : m_capture(&requestFrameCapture) {
    // Frame complete, before the guest's swap: everything that reads the frame first.
    m_recorder.addAssemblyRecordedListener(&m_poseLocator);
    // **The blend is asked which paint of the pair is in progress, from the paint module itself.**
    // Registered before anything else is because it writes into the guest's own buffer, so it has
    // to be the listener the observer reaches with a mutable span -- and it is last in that list
    // because a measurement must never see the value the blend wrote: the census reports the
    // title's own pose, and a census that read a lerp would be a census of this project's own
    // arithmetic.
    m_poseBlend.setPaint(&m_paint);
    m_recorder.addAssemblyBeforeDrawListener(&m_poseBlend);
    m_recorder.addAluConstantsListener(&m_viewBlend);
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
    m_particleProbe.install();
    m_environmentProbe.install();
    m_lineProbe.install();
    // The frame counter, wired before the install: the locator samples an object once per
    // frame, and a sample taken twice inside one frame cannot see a pose move.
    m_nodePose.setFrameCounter(&m_paint.paintCounter());
    m_blocks.setIdentity(&m_objectIdentity);
    m_recorder.setObjectIdentity(&m_objectIdentity);
    m_recorder.addDrawRecordedListener(&m_drawAttributes);
    m_vertexHistory.setFrameCounter(&m_paint.paintCounter());
    m_recorder.addDrawRecordedListener(&m_vertexHistory);
    m_blocks.setDrawAttributeCensus(&m_drawAttributes);
    m_blocks.setVertexPoseHistory(&m_vertexHistory);
    m_blockRing.setFrameCounter(&m_paint.paintCounter());
    m_blocks.setBlockRing(&m_blockRing);
    m_blocks.setBlockAddress(&m_blockAddress);
    m_recorder.setBlockAddress(&m_blockAddress);
    m_nodePose.install();
    m_paint.install();
    m_blocks.install();
    m_logic.install();
    m_viewBlend.install();
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
