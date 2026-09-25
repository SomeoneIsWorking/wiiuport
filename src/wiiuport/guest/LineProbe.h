#pragma once

#include "wiiuport/guest/BufferWriters.h"

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wiiuport::guest {

// Watches Wind Waker HD's 3D lines -- ropes and cords, `mDoExt_3DlineMat*`
// in the GameCube decompilation's m_Do_ext.cpp, recovered in setsail's
// docs/render-state.md -- and records each line's buffers into
// BufferWriters. Each line owns a vertex set of two buffer lists, flipped
// each frame; its update writes the line and hands the set to the title's
// double-buffer helper, which flushes the list just written and flips it.
// This probes that helper and keeps only the calls from the two line
// updates. A line's buffer is named by the first of its pair, fixed while
// the line lives; a line is never renewed in place, so it has no age.
// Reads guest memory as the call begins; changes nothing.
class LineProbe final : public GuestCallProbes::Probe {
  public:
    // `flushSet(set, _, count)` in the title's executable, and its first
    // instruction (`stwu r1, -0x20(r1)`).
    static constexpr uint32_t kFlushSet = 0x027ff1d8;
    static constexpr uint32_t kFlushSetFirstInstruction = 0x9421ffe0;
    static constexpr size_t kSetRegister = 3;
    static constexpr size_t kCountRegister = 5;
    // Where the calls of the two line updates, 0x025ed1bc and 0x025ec62c,
    // return.
    static constexpr uint32_t kLineUpdateReturn = 0x025edb2c;
    static constexpr uint32_t kOtherLineUpdateReturn = 0x025ed110;
    // The set: per list (count, first buffer), 8 bytes each, and the flip
    // naming the list just written. A list's buffers lie kBufferStride
    // apart, each's vertex bytes' guest address at kBufferVertices.
    static constexpr uint32_t kListBytes = 8;
    static constexpr uint32_t kFlip = 0x1c;
    static constexpr uint32_t kBufferStride = 0x250;
    static constexpr uint32_t kBufferVertices = 8 + 0x140;

    explicit LineProbe(BufferWriters& writers) : m_writers(writers) {
    }

    // Registers with the fork, to be installed when the title is linked.
    void install();

    void OnInstall(GuestCallProbes::Installation installation) override;
    void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;

  private:
    // The set's list `list` -- its count and first buffer -- or null unless
    // readable.
    static const void* listOf(uint32_t set, uint32_t list);
    // The guest address of buffer `index` of `list`, as the helper flushes it.
    static uint32_t bufferOf(const void* list, uint32_t index);

    BufferWriters& m_writers;
};

} // namespace wiiuport::guest
