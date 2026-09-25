#pragma once

#include "wiiuport/guest/RippleParticles.h"

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wiiuport::guest {

// Watches Wind Waker HD's `dPa_ripplePcallBack::draw` through its vtable
// slot and records each particle it draws into RippleParticles, with whether
// it was installed and each call it could not read. Reads the particle and
// its vertex store from guest memory as the call begins; changes nothing.
class RippleProbe final : public GuestCallProbes::Probe {
  public:
    // The vtable slot of `dPa_ripplePcallBack::draw` and the function it
    // holds in the title's executable.
    static constexpr uint32_t kDrawSlot = 0x100523c4;
    static constexpr uint32_t kDraw = 0x025a6c3c;
    // `draw(this, emitter, particle)`: the particle is the third argument.
    static constexpr size_t kParticleRegister = 5;
    // JPABaseParticle, as HD's code reads it: its global position (x, y, z),
    // its age in ticks, and its vertex store.
    static constexpr uint32_t kGlobalPosition = 0x28;
    static constexpr uint32_t kAge = 0x78;
    static constexpr uint32_t kVertexStore = 0xe0;
    static constexpr uint32_t kParticleBytes = kVertexStore + 4;
    // The vertex store: two buffers of kBufferStride bytes, each opening with
    // its vertex bytes' guest address, and the flip naming the one written.
    static constexpr uint32_t kBufferStride = 0x254;
    static constexpr uint32_t kFlip = 0x950;

    explicit RippleProbe(RippleParticles& particles) : m_particles(particles) {
    }

    // Registers with the fork, to be installed when the title is linked.
    void install();

    void OnInstall(GuestCallProbes::Installation installation) override;
    void OnCall(std::span<const uint32_t, 32> gpr) override;

  private:
    RippleParticles& m_particles;
};

} // namespace wiiuport::guest
