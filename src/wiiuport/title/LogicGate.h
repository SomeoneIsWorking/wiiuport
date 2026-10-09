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
    // The tick, and the instruction the gate continues at.
    //
    // The probe holds the entry and its stub runs the entry's own first
    // instruction, so the gate takes over at the *second*: kTickBody is
    // kTick + 4, and a call the gate lets through is the title's tick entered the
    // way the title enters it. That word is checked rather than assumed, because a
    // revision whose tick starts differently gets a refusal instead of a gate that
    // ran the wrong code -- and because the tick's epilogue reads the saved link
    // register back out of the caller's frame, so entering it anywhere but here
    // returns through a register nobody saved.
    static constexpr uint32_t kTick = 0x025d42ec;
    static constexpr uint32_t kTickFirst = 0x7c0802a6;  // mfspr r0,LR
    static constexpr uint32_t kTickSecond = 0x90010004; // stw  r0,0x4(r1)
    static constexpr uint32_t kTickBody = kTick + 4;
    // The per-frame entry, read so the report can name what it is gating.
    static constexpr uint32_t kFrameEntry = 0x025f172c;
    // The block's shape: the gate's own words. `kThroughWord` is where the gate's
    // through path starts, after the skipped path's return.
    //
    // The through path is where the ticks counter is incremented -- the path that
    // runs the tick -- followed by a branch to the word *after* the gate's
    // boundary, and the probe's stub has already run the entry's first
    // instruction, so the title runs every remaining one itself. The path used to
    // be three words that supplied the tick's prologue and branched to the word
    // after it -- correct for a gate that replaced the tick's second instruction,
    // wrong for a gate entered at it. It skipped the tick's `stw r0,0x4(r1)`, so
    // the tick returned through a link register it had never saved and the title
    // spun in a wait loop at 0x027f09d8, forever, with a control channel still
    // answering.
    //
    // And the ticks counter used to be incremented on the *skipped* path, so the
    // count the report calls ticks counted the calls that did not run.
    //
    // `kBranchWord` is separate from `kThroughWord` on purpose. The through path
    // starts at `kThroughWord` and ends in a branch at `kBranchWord`, and a
    // displacement is measured from the word it stands in: measured from the
    // start of the path instead, it lands twenty bytes past the tick's second
    // instruction, which hangs the title with the gate armed and reads as a
    // title that has stopped. The two were one constant when the through path was
    // a single word, and one is not one when it is six.
    static constexpr size_t kThroughWord = 8;
    static constexpr size_t kBranchWord = 13;
    // A skipped call runs the draw phase alone, so the in-between paint is drawn from the
    // tick's state by the title's own draw methods.
    static constexpr size_t kSkippedDrawWord = 14;
    static constexpr size_t kGateWords = 32;
    // fpcM_Management's draw half: MtxInit, then fpcDw_Handler(fpcM_DrawIterater, fpcM_Draw).
    static constexpr uint32_t kMatrixInit = 0x0200fac4;
    static constexpr uint32_t kDrawHandler = 0x025de37c;
    static constexpr uint32_t kDrawIterator = 0x025df908;
    static constexpr uint32_t kDrawProcess = 0x025de2cc;
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
    // `resume` is where the call continues: zero is the instruction after the
    // entry, and the gate passes its own block, which is the only way into the
    // trampoline area that is known to arrive.
    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry, uint32_t resume);
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

    struct Seams {
        Register registerProbe;
        AllocateCode allocateCode;
        AllocateData allocateData;
        WriteWord writeWord;
        ReadWord readWord;
    };

    // The gate's two blocks: instructions the guest executes and counters it writes.
    struct Memory {
        uint32_t code = 0;
        uint32_t counters = 0;
    };

    enum class Through : uint8_t {
        Direct = 1,
        Counter = 2
    };

    explicit LogicGate(Seams seams);

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
    static std::vector<uint32_t> throughPayload(uint32_t blockAddress, Through flavour);

    // Puts the gate's block back to its pass-through word, so the tick runs on
    // every call again. The title's own code is not touched: the probe holds the
    // entry and the block is where the decision lives.
    std::string disable();

    // Where the standing probe's call continues: the gate's block, always. That
    // is the wiring rather than a choice -- the stub's branch is fixed when the
    // probe is installed, so a call cannot be sent somewhere else afterwards --
    // and it is why the block holds a pass-through word whenever the gate is out
    // instead of being left unfilled. Zero when the gate was never wired.
    uint32_t resumeTarget() const;

    bool enabled() const {
        return m_enabled.load();
    }

    // The gate's words for a block at `blockAddress`: count the call, let every
    // other one through. Every branch is direct, and the tick is reached by a
    // tail branch so that its return goes to the title's caller.
    // Sixteen words for `blockAddress` to branch to, counting in
    // `countersAddress`: two addresses because the two are different kinds of
    // memory, instructions the guest executes and words it writes.
    static std::vector<uint32_t> payload(Memory memory);

    std::string json() const;

  private:
    void onInstalled(GuestCallProbes::Installation installation);
    void onCounted(GuestCallProbes::Installation installation);
    // Writes the gate's block as a bare pass-through to the instruction after the
    // tick's entry. True when the block took it. This is the gate's out state,
    // and it is why the block is never empty while a call can reach it.
    bool passThrough();

    Register m_register;
    AllocateCode m_allocateCode;
    AllocateData m_allocateData;
    WriteWord m_writeWord;
    ReadWord m_readWord;
    std::atomic<uint32_t> m_block{0};
    std::atomic<uint32_t> m_counters{0};
    int m_throughFlavour = 2;
    // Which payload is in, and which flavour of the pass-through control, so that
    // enabling the same thing twice is a no-op while enabling a *different* thing
    // is not. Without these, a report could say the gate is on and the block
    // could hold the control.
    bool m_through = false;
    int m_installedFlavour = 0;
    // Read by the Latte thread.
    std::atomic<bool> m_enabled{false};
    mutable std::mutex m_mutex;
    std::string m_refusal;
    // The probe exists for the link-time moment and to hold the entry while the
    // gate is out of it. Whether it *installed* is reported, because a gate whose
    // probe was refused is a gate that is counting calls nobody made, and the two
    // look the same from the outside: zeros.
    class Moment;
    Moment* m_moment = nullptr;
    // The standing probe on the tick: it counts the calls and its resume is the
    // gate's block.
    class Counter;
    Counter* m_counter = nullptr;
    std::optional<GuestCallProbes::Installation> m_counted;
    std::optional<GuestCallProbes::Installation> m_probe;
};

} // namespace wiiuport::title
