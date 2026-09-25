#include "wiiuport/guest/RippleProbe.h"

#include "wiiuport/guest/GuestWords.h"

namespace wiiuport::guest {

void RippleProbe::install() {
    GuestCallProbes::Register(kDrawSlot, kDraw, *this);
}

void RippleProbe::OnInstall(GuestCallProbes::Installation installation) {
    m_particles.noteInstalled(installation == GuestCallProbes::Installation::Installed);
}

void RippleProbe::OnCall(std::span<const uint32_t, 32> gpr) {
    const void* particle = GuestCallProbes::GuestBytes(gpr[kParticleRegister], kParticleBytes);
    const void* store =
        particle == nullptr
            ? nullptr
            : GuestCallProbes::GuestBytes(guestWord(particle, kVertexStore), kFlip + 4);
    if (store == nullptr) {
        m_particles.noteUnreadable();
        return;
    }
    uint32_t flip = guestWord(store, kFlip);
    const void* source = flip > 1 ? nullptr
                                  : GuestCallProbes::GuestBytes(
                                        guestWord(store, static_cast<size_t>(flip) * kBufferStride),
                                        RippleParticles::kQuadBytes);
    if (source == nullptr) {
        m_particles.noteUnreadable();
        return;
    }
    m_particles.record(
        source, {.object = {.address = gpr[kParticleRegister], .age = guestFloat(particle, kAge)},
                 .x = guestFloat(particle, kGlobalPosition),
                 .z = guestFloat(particle, kGlobalPosition + 8)});
}

} // namespace wiiuport::guest
