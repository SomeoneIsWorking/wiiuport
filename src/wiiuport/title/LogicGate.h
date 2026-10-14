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
// The stand-in paints twice per frame, and every paint runs the title's frame logic
// `FUN_0203593c`: the UI manager's event update, the per-frame entry and its tick
// `fapGm_Execute`, the 2D layouts and sound. The gate counts its calls and lets every other
// one through; a skipped call runs fpcM's draw half and the shadow buffer flip, so the in-between
// paint is drawn from the logic's state. Gating the tick alone left the UI manager taking one event
// per paint while the menus that read it ran at half that, and dropped the presses.
//
// It replaces the entry rather than wrapping it, because a return into the stand-in's memory
// does not arrive; both branches out of the block go to the title's own code.
class LogicGate {
  public:
    // The probe's stub runs the entry's first instruction, so a call let through continues
    // at the second, which is checked: a revision that starts differently is refused.
    static constexpr uint32_t kFrameLogic = 0x0203593c;
    static constexpr uint32_t kFrameLogicFirst = 0x7c0802a6;  // mfspr r0,LR
    static constexpr uint32_t kFrameLogicSecond = 0x9421ffe8; // stwu r1,-0x18(r1)
    static constexpr uint32_t kFrameLogicBody = kFrameLogic + 4;
    // The per-frame entry, read so the report can name what it is gating.
    static constexpr uint32_t kFrameEntry = 0x025f172c;
    // The through path counts a tick and branches to kFrameLogicBody from kBranchWord; a
    // displacement is measured from the word it stands in, not from kThroughWord.
    static constexpr size_t kThroughWord = 8;
    static constexpr size_t kBranchWord = 13;
    // A skipped call runs the draw phase alone, so the in-between paint is drawn from the
    // logic's state by the title's own draw methods. It marks itself in the skipping word.
    // It then flips the shadow prepass's per-frame buffers as the frame logic does after
    // every draw, or the next paint writes the slot the GPU still reads for this one.
    static constexpr size_t kSkippedDrawWord = 14;
    static constexpr size_t kGateWords = 53;
    // The frame logic's guard and call, lifted from 0x020359f0: FUN_0278febc(layer + 0x15d4)
    // for the shadow layer at **(*0x101f95d0 + 0x1024).
    static constexpr uint32_t kShadowFlipGuard = 0x020359f0;
    static constexpr uint32_t kShadowFlip = 0x0278febc;
    // dScnPly_Draw (0x025af8a0) advances the tick before its draw loop: collision, grass and
    // trees, particles, the frame counter 0x101ff560. A skipped call's draw phase goes from
    // the scene's view straight to the loop; no register set in between is read after it.
    static constexpr uint32_t kSceneWork = 0x025af934;
    static constexpr uint32_t kSceneWorkFirst = 0x386326a4; // addi r3,r3,0x26a4
    static constexpr uint32_t kSceneDrawLoop = 0x025b0234;
    static constexpr size_t kSceneGateWord = kGateWords;
    static constexpr size_t kSceneGateWords = 7;
    // The controller manager's calc (0x0273cbd0) reads the pads and turns holds into presses.
    // It runs only when the logic has ticked since it last ran, so no press lands on a paint
    // whose logic is skipped.
    static constexpr uint32_t kControllerCalc = 0x0273cbd0;
    static constexpr uint32_t kControllerCalcFirst = 0x7c0802a6;  // mfspr r0,LR
    static constexpr uint32_t kControllerCalcSecond = 0x9421ffe8; // stwu r1,-0x18(r1)
    static constexpr uint32_t kControllerCalcBody = kControllerCalc + 4;
    static constexpr size_t kControllerGateWord = kSceneGateWord + kSceneGateWords;
    static constexpr size_t kControllerGateWords = 11;
    // fpcM_Management's draw half: MtxInit, then fpcDw_Handler(fpcM_DrawIterater, fpcM_Draw).
    static constexpr uint32_t kMatrixInit = 0x0200fac4;
    static constexpr uint32_t kDrawHandler = 0x025de37c;
    static constexpr uint32_t kDrawIterator = 0x025df908;
    static constexpr uint32_t kDrawProcess = 0x025de2cc;
    // The code block holds only the sixteen instructions; the two counters the
    // guest writes are in a block of their own, in memory it may write.
    static constexpr size_t kBlockWords = kGateWords + kSceneGateWords + kControllerGateWords;
    static constexpr size_t kCallsWord = 0;
    static constexpr size_t kTicksWord = 1;
    // Non-zero while a skipped call's draw phase runs.
    static constexpr size_t kSkippingWord = 2;
    // The tick count the controller calc last ran at.
    static constexpr size_t kControllerTicksWord = 3;
    static constexpr size_t kCounterWords = 4;
    // How many bytes the gate asks the loader's arena for, reported so a report
    // that says the counters are zero can be read against the space they are in.
    static constexpr uint32_t kBlockBytes = 4 * kBlockWords;
    static constexpr uint32_t kCounterBytes = 4 * kCounterWords;

    // The fork's seams, injected so this is testable without a guest. The last
    // argument says the probe does not keep the frame logic's entry: this one wants the
    // moment the title was linked and nothing after; holding the entry would take it from
    // the caller census for the rest of the run.
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
    // `through` installs a payload that only branches back to the frame logic and keeps no
    // state: the control for whether a branch out of recompiled code into the loader's arena
    // runs, observed by the caller census.
    std::string enable(bool through = false, int throughFlavour = 2);

    // The pass-through control: a payload of one word (direct branch) or four
    // (through the count register) that does nothing but let the frame logic run. Which
    // one is the experiment, and the census is the observer.
    static std::vector<uint32_t> throughPayload(uint32_t blockAddress, Through flavour);

    // Puts the gate's block back to its pass-through word, so the frame logic runs
    // on every call again. The title's own code is not touched: the probe holds the
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
    // other one through. Every branch is direct, and the frame logic is reached
    // by a tail branch so that its return goes to the title's caller.
    // Sixteen words for `blockAddress` to branch to, counting in
    // `countersAddress`: two addresses because the two are different kinds of
    // memory, instructions the guest executes and words it writes.
    static std::vector<uint32_t> payload(Memory memory);

    // The scene gate's words: in a skipped call's draw phase, the scene's draw continues at its
    // draw loop; otherwise at the instruction after kSceneWork. Empty when out of reach.
    static std::vector<uint32_t> scenePayload(Memory memory);

    // The controller gate's words: the calc runs when the tick count differs from the one it last
    // ran at, and otherwise returns. Empty when out of reach.
    static std::vector<uint32_t> controllerPayload(Memory memory);

    std::string json() const;

  private:
    void onInstalled(GuestCallProbes::Installation installation);
    void onCounted(GuestCallProbes::Installation installation);
    // Writes the gate's block as a bare pass-through to the instruction after the
    // frame logic's entry. True when the block took it. This is the gate's out state,
    // and it is why the block is never empty while a call can reach it.
    bool passThrough();
    // The controller gate's out state: a branch to the calc's own body.
    bool controllerPassThrough();

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
    // The standing probe on the frame logic: it counts the calls and its resume is the
    // gate's block.
    class Counter;
    Counter* m_counter = nullptr;
    // A standing probe whose resume is one of the gate's own blocks.
    class Held;
    Held* m_sceneWork = nullptr;
    Held* m_controllerCalc = nullptr;
    std::optional<GuestCallProbes::Installation> m_sceneProbe;
    std::optional<GuestCallProbes::Installation> m_controllerProbe;
    std::optional<GuestCallProbes::Installation> m_counted;
    std::optional<GuestCallProbes::Installation> m_probe;
};

} // namespace wiiuport::title
