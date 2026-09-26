#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's logic, gated on its own count instead of the flip.
//
// Measured on the real title: with the flip at one vblank, the display painted
// 46.17 a second and the logic ticked 46.17 a second -- 277 paints and 277 calls
// to `fapGm_Execute` in the same six-second window, one for one. The tick is not
// merely correlated with the flip, it is called once per paint.
//
// Read out of the title's own code, `FUN_025f172c` is the per-frame entry and it
// calls the tick unconditionally:
//
//     uVar1 = DAT_1048d0ac;                      /* a period */
//     DAT_1048d0a8 = DAT_1048d0a8 + 1;            /* a counter  */
//     if (uVar1 != 0 && DAT_1048d0a8 == (DAT_1048d0a8 / uVar1) * uVar1) {
//         FUN_025f1654();                         /* every Nth frame */
//     }
//     FUN_025f2d74(); FUN_025e15e0(); FUN_025d42ec();   /* the tick, always */
//
// So the title already counts frames and already has a period, and uses both for
// something else. The gate is the same shape applied to the tick: count the
// calls, and let an even one through. Nothing is invented here -- the count, the
// test and the skip are the title's own idiom, applied to the one call that
// should have obeyed them.
//
// It replaces the tick's entry rather than wrapping it, because a wrapper would
// have to *return into itself* and a return into the stand-in's memory does not
// work here: both the branch out and the return go to the title's own code, which
// is the only direction measured to hold.
class LogicGate {
  public:
    // The tick, and the instruction after the one the probe replaced. The gate
    // takes *that* word for its branch, not the entry: the entry is the probe's,
    // and the tick cannot be entered past its prologue because its epilogue reads
    // the saved link register back out of the caller's frame and returns through
    // it. So the gate supplies the prologue itself and branches to the body after
    // it -- which is why kTickSecond is the word it checks for.
    static constexpr uint32_t kTick = 0x025d42ec;
    static constexpr uint32_t kTickFirst = 0x7c0802a6;  // mfspr r0,LR
    static constexpr uint32_t kTickSecond = 0x90010004; // stw  r0,0x4(r1)
    static constexpr uint32_t kTickBody = kTick + 4;
    // The per-frame entry, read so the report can name what it is gating.
    static constexpr uint32_t kFrameEntry = 0x025f172c;
    // The block's shape: the gate's own words, then the two counters it keeps.
    // Both counters are in guest memory, so the host can read the logic's rate
    // and the call rate without a probe. `kThroughWord` is where the gate's
    // through path starts, after the skipped path's return.
    static constexpr size_t kThroughWord = 13;
    static constexpr size_t kGateWords = 16;
    static constexpr size_t kCallsWord = 16;
    static constexpr size_t kTicksWord = 17;
    static constexpr size_t kBlockWords = 18;

    // The fork's seams, injected so this is testable without a guest.
    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe);
    using AllocateCode = uint32_t (*)(uint32_t sizeInBytes);
    using WriteWord = bool (*)(uint32_t guestAddress, uint32_t value);
    using ReadWord = bool (*)(uint32_t guestAddress, uint32_t& value);
    // For the report: the call the gate replaced, and the counter's address.
    using Where = std::string (*)();

    LogicGate(Register registerProbe, AllocateCode allocateCode, WriteWord writeWord,
              ReadWord readWord);

    // Registers the probe, which is also the moment the title's modules are
    // linked and the gate's memory can be taken.
    void install();

    // Puts the gate in. Empty on success, otherwise the refusal by cause.
    std::string enable();

    // Puts the tick's own entry back.
    std::string disable();

    bool enabled() const {
        return m_enabled;
    }

    // The gate's words for a block at `blockAddress`: count the call, let every
    // other one through. Every branch is direct, and the tick is reached by a
    // tail branch so that its return goes to the title's caller.
    static std::vector<uint32_t> payload(uint32_t blockAddress);

    std::string json() const;

  private:
    void onInstalled();

    Register m_register;
    AllocateCode m_allocateCode;
    WriteWord m_writeWord;
    ReadWord m_readWord;
    std::atomic<uint32_t> m_block{0};
    uint32_t m_original = 0;
    bool m_enabled = false;
    mutable std::mutex m_mutex;
    std::string m_refusal;
    // The probe exists only for the link-time moment, and its counter is the
    // gate's, so the calls it saw are not a measurement worth keeping.
    class Moment;
    Moment* m_moment = nullptr;
};

} // namespace wiiuport::title
