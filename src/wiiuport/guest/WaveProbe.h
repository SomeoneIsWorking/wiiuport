#pragma once

#include "wiiuport/guest/BufferWriters.h"

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wiiuport::guest {

// Watches Wind Waker HD's sea waves -- the white crests of the environment's
// wave packet (`drawWave` of JSystem-era `d_kankyo_rain`, recovered in
// setsail's docs/render-state.md) -- and records each wave and its quad into
// BufferWriters. HD writes each wave's quad into the wave's own two vertex
// buffers and flushes it through the title's vertex-buffer flush; this
// probes that flush and keeps only the calls from the wave writer's one call
// site, where the writer's registers hold the packet and the wave's index.
// Reads guest memory as the call begins; changes nothing.
class WaveProbe final : public GuestCallProbes::Probe {
  public:
    // `flush(buffer, offset, size)` in the title's executable, and its first
    // instruction (`lwz r12, 0x140(r3)`: the buffer's vertex bytes).
    static constexpr uint32_t kFlush = 0x027b5e94;
    static constexpr uint32_t kFlushFirstInstruction = 0x81830140;
    static constexpr size_t kBufferRegister = 3;
    static constexpr size_t kOffsetRegister = 4;
    static constexpr uint32_t kBufferVertices = 0x140;
    // The wave writer's flush returns here, with the wave packet and the
    // wave's index in these of its registers.
    static constexpr uint32_t kWaveFlushReturn = 0x02575488;
    static constexpr size_t kPacketRegister = 30;
    static constexpr size_t kIndexRegister = 23;
    // The packet's waves: the first's place, the stride between them, and a
    // wave's counter from its place -- which only rises while the wave
    // lives, the wave's age.
    static constexpr uint32_t kWaves = 0xa0;
    static constexpr uint32_t kWaveStride = 0x38;
    static constexpr uint32_t kCounter = 0x24;

    explicit WaveProbe(BufferWriters& writers) : m_writers(writers) {
    }

    // Registers with the fork, to be installed when the title is linked.
    void install();

    void OnInstall(GuestCallProbes::Installation installation) override;
    void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;

  private:
    BufferWriters& m_writers;
};

} // namespace wiiuport::guest
