#include "wiiuport/title/CameraCuts.h"

#include "wiiuport/guest/ProbeInstallation.h"
#include "wiiuport/title/JsonBody.h"

#include <utility>

namespace wiiuport::title {

void CameraCuts::Paragraphs::OnInstall(GuestCallProbes::Installation value) {
    installation = value;
}

void CameraCuts::Paragraphs::OnCall(std::span<const uint32_t, 32> gpr, uint32_t /*returnAddress*/) {
    m_owner.onSetVariableValue(gpr[kAdaptorRegister]);
}

CameraCuts::CameraCuts(Seams seams)
    : m_register(seams.registerProbe), m_readWords(std::move(seams.readWords)) {
}

void CameraCuts::install() {
    m_register(kSetVariableValue, kSetVariableValueFirst, m_paragraphs, true, 0);
}

std::optional<uint32_t> CameraCuts::readWord(uint32_t address) const {
    uint32_t word = 0;
    if (!m_readWords(address, &word, 1)) {
        return std::nullopt;
    }
    return word;
}

std::optional<uint32_t> CameraCuts::activeDemoCamera() const {
    std::optional<uint32_t> demo = readWord(kDemoObject);
    if (!demo || *demo == 0) {
        return std::nullopt;
    }
    std::optional<uint32_t> camera = readWord(*demo + kActiveCamera);
    if (!camera || *camera == 0) {
        return std::nullopt;
    }
    return camera;
}

void CameraCuts::onSetVariableValue(uint32_t adaptor) {
    std::optional<uint32_t> camera = activeDemoCamera();
    if (!camera) {
        return;
    }
    if (readWord(adaptor + kAdaptorStageObject) == camera) {
        m_demoParagraphs.fetch_add(1, std::memory_order_relaxed);
        m_demoParagraph.store(true, std::memory_order_relaxed);
    }
}

bool CameraCuts::startsShot(uint32_t camera) {
    if (m_demoParagraph.exchange(false, std::memory_order_relaxed) && activeDemoCamera()) {
        m_demoShots.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    std::optional<uint32_t> play = readWord(kEventCameraPlay);
    std::optional<uint32_t> cutTicks = readWord(camera + kCameraBody + kCutTicks);
    if (play && *play != 0 && cutTicks == 1u) {
        m_eventShots.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

std::string CameraCuts::json() const {
    JsonBody body;
    body.string("probe", std::string(guest::installationName(m_paragraphs.installation.load())));
    body.number("demoParagraphs", m_demoParagraphs.load());
    body.number("demoShots", m_demoShots.load());
    body.number("eventShots", m_eventShots.load());
    return body.finish();
}

} // namespace wiiuport::title
