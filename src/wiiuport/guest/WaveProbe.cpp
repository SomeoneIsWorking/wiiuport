#include "wiiuport/guest/WaveProbe.h"

#include "wiiuport/guest/GuestWords.h"

#include <cstring>

namespace wiiuport::guest {

void WaveProbe::install() {
    GuestCallProbes::Register(kFlush, kFlushFirstInstruction, *this);
}

void WaveProbe::OnInstall(GuestCallProbes::Installation installation) {
    if (installation == GuestCallProbes::Installation::Installed) {
        m_particles.noteInstalled();
    }
}

void WaveProbe::OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) {
    if (returnAddress != kWaveFlushReturn) {
        return;
    }
    uint32_t address = gpr[kPacketRegister] + kWaves + (gpr[kIndexRegister] * kWaveStride);
    const void* wave = GuestCallProbes::GuestBytes(address, kCounter + 4);
    const void* buffer = GuestCallProbes::GuestBytes(gpr[kBufferRegister], kBufferVertices + 4);
    const void* source =
        buffer == nullptr
            ? nullptr
            : GuestCallProbes::GuestBytes(guestWord(buffer, kBufferVertices) + gpr[kOffsetRegister],
                                          Particles::kQuadBytes);
    if (wave == nullptr || source == nullptr) {
        m_particles.noteUnreadable();
        return;
    }
    Particles::Quad quad{};
    std::memcpy(quad.data(), source, quad.size());
    m_particles.record(source, {.address = address, .age = guestFloat(wave, kCounter)}, quad);
}

} // namespace wiiuport::guest
