#pragma once

#include "wiiuport/guest/Particles.h"

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wiiuport::guest {

// Watches Wind Waker HD's particle commit -- the call every particle writer
// makes once it has written a particle's quad, which then flips the
// particle's vertex store -- and records each particle and its quad into
// Particles. Reads guest memory as the call begins; changes nothing.
class ParticleProbe final : public GuestCallProbes::Probe {
  public:
    // `commit(particle, store)` in the title's executable, and its first
    // instruction (`mfspr r0, lr`).
    static constexpr uint32_t kCommit = 0x02825158;
    static constexpr uint32_t kCommitFirstInstruction = 0x7c0802a6;
    // Its arguments: the particle, and 0 for the particle's own quad store
    // (another value commits a second store some shapes keep).
    static constexpr size_t kParticleRegister = 3;
    static constexpr size_t kStoreRegister = 4;
    // JPABaseParticle, as HD's code reads it: its age in ticks, whether it
    // is drawn (a byte), and its vertex store.
    static constexpr uint32_t kAge = 0x78;
    static constexpr uint32_t kDrawn = 0x122;
    static constexpr uint32_t kVertexStore = 0xe0;
    static constexpr uint32_t kParticleBytes = kDrawn + 1;
    // The vertex store: two buffers of kBufferStride bytes, each opening with
    // its vertex bytes' guest address, and the flip naming the one written.
    static constexpr uint32_t kBufferStride = 0x254;
    static constexpr uint32_t kFlip = 0x950;

    explicit ParticleProbe(Particles& particles) : m_particles(particles) {
    }

    // Registers with the fork, to be installed when the title is linked.
    void install();

    void OnInstall(GuestCallProbes::Installation installation) override;
    void OnCall(std::span<const uint32_t, 32> gpr) override;

  private:
    Particles& m_particles;
};

} // namespace wiiuport::guest
