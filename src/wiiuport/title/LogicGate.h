#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
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
    // The code block holds only the sixteen instructions; the two counters the
    // guest writes are in a block of their own, in memory it may write.
    static constexpr size_t kBlockWords = kGateWords;
    static constexpr size_t kCallsWord = 0;
    static constexpr size_t kTicksWord = 1;
    static constexpr size_t kCounterWords = 2;
    // How many bytes the gate asks the loader's arena for, reported so a report
    // that says the counters are zero can be read against the space they are in.
    static constexpr uint32_t kBlockBytes = 4 * kBlockWords;
    static constexpr uint32_t kCounterBytes = 4 * kCounterWords;

    // The fork's seams, injected so this is testable without a guest. The last
    // argument says the probe does not keep the tick's entry: this one wants the
    // moment the title was linked and nothing after, and holding the entry would
    // take it from the caller census for the rest of the run -- which showed up as
    // the tick being counted zero times while the display thread painted 1479.
    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry);
    using AllocateCode = uint32_t (*)(uint32_t sizeInBytes);
    // The counters are words the *guest* stores into, which is a different kind of
    // memory from words it executes: they were in the code block, and a guest
    // store into an area documented for instructions is not something to rely on,
    // because a store that lands nowhere is indistinguishable from code that never
    // ran -- which is exactly what the counters said.
    using AllocateData = uint32_t (*)(uint32_t sizeInBytes);
    using WriteWord = bool (*)(uint32_t guestAddress, uint32_t value);
    using ReadWord = bool (*)(uint32_t guestAddress, uint32_t& value);
    // For the report: the call the gate replaced, and the counter's address.
    using Where = std::string (*)();

    LogicGate(Register registerProbe, AllocateCode allocateCode, AllocateData allocateData,
              WriteWord writeWord, ReadWord readWord);

    // Registers the probe, which is also the moment the title's modules are
    // linked and the gate's memory can be taken.
    void install();

    // Puts the gate in. Empty on success, otherwise the refusal by cause.
    // `through` installs a payload that only branches back to the tick and keeps
    // no state at all: the control for whether a direct branch out of recompiled
    // code into the loader's arena runs. Everything else about the install is
    // identical, and the observer is the caller census, which keeps counting the
    // tick either way -- so a tick rate that holds says the branch executed and a
    // tick rate that stops says it did not, with no counter of the gate's own
    // involved and so nothing resting on the gate's payload.
    std::string enable(bool through = false, int throughFlavour = 2);

    // The pass-through control: a payload of one word (direct branch) or four
    // (through the count register) that does nothing but let the tick run. Which
    // one is the experiment, and the census is the observer.
    static std::vector<uint32_t> throughPayload(uint32_t blockAddress, int flavour);

    // Puts the tick's own entry back.
    std::string disable();

    bool enabled() const {
        return m_enabled;
    }

    // The gate's words for a block at `blockAddress`: count the call, let every
    // other one through. Every branch is direct, and the tick is reached by a
    // tail branch so that its return goes to the title's caller.
    // Sixteen words for `blockAddress` to branch to, counting in
    // `countersAddress`: two addresses because the two are different kinds of
    // memory, instructions the guest executes and words it writes.
    static std::vector<uint32_t> payload(uint32_t blockAddress, uint32_t countersAddress);

    std::string json() const;

  private:
    void onInstalled(GuestCallProbes::Installation installation);

    Register m_register;
    AllocateCode m_allocateCode;
    AllocateData m_allocateData;
    WriteWord m_writeWord;
    ReadWord m_readWord;
    std::atomic<uint32_t> m_block{0};
    std::atomic<uint32_t> m_counters{0};
    uint32_t m_original = 0;
    int m_throughFlavour = 2;
    bool m_enabled = false;
    mutable std::mutex m_mutex;
    std::string m_refusal;
    // The probe exists for the link-time moment and to hold the entry while the
    // gate is out of it. Whether it *installed* is reported, because a gate whose
    // probe was refused is a gate that is counting calls nobody made, and the two
    // look the same from the outside: zeros.
    class Moment;
    Moment* m_moment = nullptr;
    std::optional<GuestCallProbes::Installation> m_probe;
};

} // namespace wiiuport::title
