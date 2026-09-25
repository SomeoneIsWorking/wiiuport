#pragma once

#include "wiiuport/guest/BufferWriters.h"

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace wiiuport::guest {

// Watches Wind Waker HD's environment effects -- the sea's wave crests and
// the sky's cloud cards, the wave and vrkumo packets of the GameCube
// decompilation's d_kankyo_rain.cpp, recovered in setsail's
// docs/render-state.md -- and records each one's quad into BufferWriters.
// HD writes each wave's and each cloud card's quad into two vertex buffers
// of its own and flushes them through the title's vertex-buffer flush; this
// probes that flush and keeps only the calls from those writers' call
// sites, whose registers name the object. Reads guest memory as the call
// begins; changes nothing.
class EnvironmentProbe final : public GuestCallProbes::Probe {
  public:
    // `flush(buffer, offset, size)` in the title's executable, and its first
    // instruction (`lwz r12, 0x140(r3)`: the buffer's vertex bytes).
    static constexpr uint32_t kFlush = 0x027b5e94;
    static constexpr uint32_t kFlushFirstInstruction = 0x81830140;
    static constexpr size_t kBufferRegister = 3;
    static constexpr size_t kOffsetRegister = 4;
    static constexpr uint32_t kBufferVertices = 0x140;

    // The wave writer's flush returns here, with the wave packet and the
    // wave's index in these of its registers. The packet's waves: the
    // first's place, the stride between them, and a wave's counter from its
    // place -- which only rises while the wave lives, the wave's age.
    static constexpr uint32_t kWaveFlushReturn = 0x02575488;
    static constexpr size_t kWavePacketRegister = 30;
    static constexpr size_t kWaveIndexRegister = 23;
    static constexpr uint32_t kWaves = 0xa0;
    static constexpr uint32_t kWaveStride = 0x38;
    static constexpr uint32_t kWaveCounter = 0x24;

    // The cloud writer flushes its two groups of 300 cards' buffers after
    // drawing, returning here for each group, with the group's flip -- which
    // of each card's two buffers was written -- at the address in this
    // register. A card's buffers are kCardBufferStride apart; the first
    // names the card, which is moved rather than renewed, so has no age.
    static constexpr uint32_t kCloudFlushReturn = 0x02576ff4;
    static constexpr uint32_t kOtherCloudFlushReturn = 0x02577040;
    static constexpr size_t kCloudFlipRegister = 31;
    static constexpr uint32_t kCardBufferStride = 0x254;

    explicit EnvironmentProbe(BufferWriters& writers) : m_writers(writers) {
    }

    // Registers with the fork, to be installed when the title is linked.
    void install();

    void OnInstall(GuestCallProbes::Installation installation) override;
    void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;

  private:
    // The wave or cloud card the flush's writer names, or none.
    static std::optional<interp::GuestObject> objectOf(std::span<const uint32_t, 32> gpr,
                                                       uint32_t returnAddress);

    BufferWriters& m_writers;
};

} // namespace wiiuport::guest
