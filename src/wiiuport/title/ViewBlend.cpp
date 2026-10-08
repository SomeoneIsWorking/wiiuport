#include "wiiuport/title/ViewBlend.h"

#include "wiiuport/guest/ProbeInstallation.h"
#include "wiiuport/interp/Blendable.h"
#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace wiiuport::title {

ViewBlend::ViewBlend(Register registerProbe, ReadGuest readGuest,
                     CommandStreamIdentity::ReadPosition readPosition, InBetween inBetween)
    : m_register(registerProbe), m_readGuest(std::move(readGuest)),
      m_readPosition(std::move(readPosition)), m_inBetween(std::move(inBetween)) {
}

void ViewBlend::install() {
    m_register(kUploadView, kUploadViewFirstInstruction, *this, true, 0);
}

void ViewBlend::OnInstall(GuestCallProbes::Installation installation) {
    std::scoped_lock lock(m_mutex);
    m_installation = installation;
}

void ViewBlend::OnCall(std::span<const uint32_t, 32> gpr, uint32_t /*returnAddress*/) {
    const uint32_t context = gpr[kContextRegister];
    const auto* flag = static_cast<const uint8_t*>(m_readGuest(context + kUploadedFlag, 1));
    const bool inBetween = m_inBetween();
    const CommandPosition at = m_readPosition();
    std::scoped_lock lock(m_mutex);
    m_report.calls++;
    if (flag == nullptr) {
        m_report.unreadable++;
        return;
    }
    if (*flag != 0) {
        m_report.alreadyUploaded++;
        return;
    }
    if (at.write == 0 || at.write < at.bufferStart || at.write >= at.bufferEnd) {
        m_report.withoutBuffer++;
        return;
    }
    if (m_pending.size() >= kMaxPending) {
        dropOldestPending();
    }
    m_pending[at.write + kPacketHeaderBytes] = {context, inBetween, ++m_sequence};
}

void ViewBlend::dropOldestPending() {
    auto oldest =
        std::min_element(m_pending.begin(), m_pending.end(), [](const auto& a, const auto& b) {
            return a.second.sequence < b.second.sequence;
        });
    m_pending.erase(oldest);
    m_report.pendingDropped++;
}

bool ViewBlend::onAluConstants(const LatteFrameHooks::AluConstants& constants) {
    std::scoped_lock lock(m_mutex);
    const auto pending = m_pending.find(constants.packet);
    if (pending == m_pending.end()) {
        return false;
    }
    const Pending upload = pending->second;
    m_pending.erase(pending);
    m_report.matched++;
    if (constants.count != kWords) {
        m_report.wrongSize++;
        return false;
    }
    std::array<float, kWords> view{};
    std::memcpy(view.data(), constants.values, sizeof(view));
    const auto held = m_held.find(upload.context);
    if (!upload.inBetween) {
        if (held == m_held.end() && m_held.size() >= kMaxContexts) {
            m_report.refusedForRoom++;
            return false;
        }
        m_held[upload.context] = view;
        m_report.held++;
        return false;
    }
    if (held == m_held.end()) {
        m_report.firstSight++;
        return false;
    }
    std::array<float, kWords> blended{};
    if (!interp::midpoint(held->second, view, blended)) {
        m_report.refusedUnblendable++;
        return false;
    }
    std::memcpy(constants.values, blended.data(), sizeof(blended));
    m_report.lerped++;
    if (!interp::sameBits(held->second, view)) {
        m_report.moved++;
    }
    return true;
}

ViewBlend::Report ViewBlend::report() const {
    std::scoped_lock lock(m_mutex);
    Report out = m_report;
    out.contexts = m_held.size();
    return out;
}

std::string ViewBlend::json() const {
    std::optional<GuestCallProbes::Installation> installation;
    {
        std::scoped_lock lock(m_mutex);
        installation = m_installation;
    }
    const Report r = report();
    JsonBody body;
    body.string("probe", std::string(guest::installationName(installation)));
    body.number("calls", r.calls);
    body.number("alreadyUploaded", r.alreadyUploaded);
    body.number("unreadable", r.unreadable);
    body.number("withoutBuffer", r.withoutBuffer);
    body.number("pendingDropped", r.pendingDropped);
    body.number("matched", r.matched);
    body.number("wrongSize", r.wrongSize);
    body.number("held", r.held);
    body.number("lerped", r.lerped);
    body.number("moved", r.moved);
    body.number("firstSight", r.firstSight);
    body.number("refusedUnblendable", r.refusedUnblendable);
    body.number("refusedForRoom", r.refusedForRoom);
    body.number("contexts", r.contexts);
    return body.finish();
}

} // namespace wiiuport::title
