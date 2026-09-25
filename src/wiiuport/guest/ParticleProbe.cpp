#include "wiiuport/guest/ParticleProbe.h"

#include "wiiuport/guest/GuestWords.h"

#include <cstring>

namespace wiiuport::guest {

void ParticleProbe::install() {
    GuestCallProbes::Register(kCommit, kCommitFirstInstruction, *this);
}

void ParticleProbe::OnInstall(GuestCallProbes::Installation installation) {
    if (installation == GuestCallProbes::Installation::Installed) {
        m_writers.noteInstalled();
    }
}

void ParticleProbe::OnCall(std::span<const uint32_t, 32> gpr, uint32_t /*returnAddress*/) {
    if (gpr[kStoreRegister] != 0) {
        return;
    }
    uint32_t address = gpr[kParticleRegister];
    const void* particle = GuestCallProbes::GuestBytes(address, kParticleBytes);
    if (particle != nullptr && static_cast<const std::byte*>(particle)[kDrawn] == std::byte{0}) {
        // Not drawn: the commit flips nothing, and no quad was written.
        return;
    }
    const void* store =
        particle == nullptr
            ? nullptr
            : GuestCallProbes::GuestBytes(guestWord(particle, kVertexStore), kFlip + 4);
    uint32_t flip = store == nullptr ? 0 : guestWord(store, kFlip);
    const void* source = store == nullptr || flip > 1
                             ? nullptr
                             : GuestCallProbes::GuestBytes(
                                   guestWord(store, static_cast<size_t>(flip) * kBufferStride),
                                   BufferWriters::kLeadingBytes);
    if (source == nullptr) {
        m_writers.noteUnreadable();
        return;
    }
    BufferWriters::Leading quad{};
    std::memcpy(quad.data(), source, quad.size());
    m_writers.record(source, {.address = address, .age = guestFloat(particle, kAge)}, quad);
}

} // namespace wiiuport::guest
