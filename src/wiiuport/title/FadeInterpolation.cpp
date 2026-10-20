#include "wiiuport/title/FadeInterpolation.h"

#include "wiiuport/guest/ProbeInstallation.h"
#include "wiiuport/title/JsonBody.h"

#include <utility>

namespace wiiuport::title {

void FadeInterpolation::Entry::OnInstall(GuestCallProbes::Installation value) {
    std::scoped_lock lock(m_owner.m_mutex);
    installation = value;
}

void FadeInterpolation::Entry::OnCall(std::span<const uint32_t, 32> gpr,
                                      uint32_t /*returnAddress*/) {
    m_owner.onFaderDraw(gpr[kManagerRegister]);
}

FadeInterpolation::FadeInterpolation(Seams seams)
    : m_register(seams.registerProbe), m_readWords(std::move(seams.readWords)),
      m_writeWords(std::move(seams.writeWords)) {
}

void FadeInterpolation::install() {
    m_register(kFaderDraw, kFaderDrawFirst, m_faderDraw, true, 0);
}

void FadeInterpolation::onFaderDraw(uint32_t manager) {
    std::scoped_lock lock(m_mutex);
    m_draws++;
    uint32_t fader = 0;
    uint32_t status = 0;
    uint32_t color = 0;
    if (!m_readWords(manager + kFader, &fader, 1) || fader == 0 ||
        !m_readWords(fader + kStatus, &status, 1) || !m_readWords(fader + kColor, &color, 1)) {
        m_unreadable++;
        return;
    }
    uint32_t shown = status == kClear ? 0 : color & kAlphaMask;
    if (!m_inMidPaint) {
        m_seen = {.tick = m_tick, .fader = fader, .alpha = shown};
        return;
    }
    if (m_restore) {
        // A second draw in this paint: already at its midpoint.
        return;
    }
    if (m_seen.tick + 1 != m_tick || m_seen.fader != fader) {
        m_firstSeen++;
        return;
    }
    uint32_t mid = (m_seen.alpha + shown) / 2;
    uint32_t blended = (color & ~kAlphaMask) | mid;
    if (!m_writeWords(fader + kColor, &blended, 1)) {
        m_writeFailures++;
        return;
    }
    m_restore = Restore{.address = fader + kColor, .color = color};
    m_blends++;
}

void FadeInterpolation::onMidPaintBegin(uint64_t tick) {
    std::scoped_lock lock(m_mutex);
    m_inMidPaint = true;
    m_tick = tick;
}

void FadeInterpolation::onMidPaintEnd() {
    std::scoped_lock lock(m_mutex);
    m_inMidPaint = false;
    if (!m_restore) {
        return;
    }
    if (m_writeWords(m_restore->address, &m_restore->color, 1)) {
        m_restored++;
    } else {
        m_writeFailures++;
    }
    m_restore.reset();
}

std::string FadeInterpolation::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("faderDraw", std::string(guest::installationName(m_faderDraw.installation)));
    body.number("draws", m_draws);
    body.number("blends", m_blends);
    body.number("firstSeen", m_firstSeen);
    body.number("unreadable", m_unreadable);
    body.number("writeFailures", m_writeFailures);
    body.number("restored", m_restored);
    return body.finish();
}

} // namespace wiiuport::title
