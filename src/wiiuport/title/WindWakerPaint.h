#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's display thread, painting each tick's world twice.
//
// The title runs its logic on one thread and paints on another, and the
// display thread's whole entry point is eleven instructions: read its vtable,
// call one slot, wait for the flip, again. Nothing between two iterations but
// that wait, and the wait is satisfied by however many vblanks the flip takes.
// So the rate of the picture is this thread's swap interval and how many times
// it paints, and nothing else -- which is all this changes. It stands in for
// the frame once, so the loop paints the same tree twice, and it asks for one
// vblank per flip where the title asks for two.
//
// The stand-in is the title's own loop body, twice, with the game's own
// `GX2SetSwapInterval` called ahead of it, then a return: every word is one
// the title executes itself, except the branch, whose displacement is worked
// out from where the block lands. The frame is re-read from the title's vtable
// on each pass, so this follows whichever display class the title installed
// rather than assuming one, and the argument is the same one the original call
// was handed.
//
// Nothing here blends. The second paint draws the same world as the first,
// which is the null case any blend has to be measured against, and the picture
// the player sees at 60 Hz is 60 copies of the title's own 30 Hz frames until
// a blend is added to the second pass.
class WindWakerPaint {
  public:
    // The display vtable this title installs, and the slot its thread calls:
    // read out of the title's image, where slot 0xcc of the vtable at
    // 0x10004e88 holds the frame at 0x0274c264.
    static constexpr uint32_t kDisplayVTable = 0x10004e88;
    static constexpr uint32_t kFrameSlot = 0xcc;
    // The frame the stand-in is written for. Refused by name rather than
    // installed over, so a different revision of the title cannot be patched
    // with this one's payload.
    static constexpr uint32_t kDisplayFrame = 0x0274c264;
    // The top of the display thread's loop, which is what the stand-in returns
    // to. It cannot return with `blr`: the game's own call is the only thing
    // that set the link register, and the stand-in's own call to ask for one
    // vblank per flip overwrites it.
    //
    // It branches to the loop, not to the thread's entry at 0x0274c00c. The
    // entry is a prologue -- `mfspr r0,LR`, a new stack frame, `or r31,r3,r3`
    // -- so re-entering it re-frames the stack on every paint and takes the
    // display pointer out of the frame the loop was using. Measured: a stand-in
    // that branched to the entry ran for seven seconds and then faulted at
    // 0x0274c020 with r31 zero, which is `lwz r12,0x24(r31)` on a null display.
    static constexpr uint32_t kDisplayLoopTop = 0x0274c020;
    // The instruction the loop starts on, checked before anything is written:
    // it is the load of the vtable, and a revision that put something else
    // there is not this title's loop.
    static constexpr uint32_t kDisplayLoopTopFirst = 0x819f0024;
    // The frame's first instruction, as GuestCallProbes relocation needs it,
    // and the gx2 import that sets the flip interval.
    static constexpr uint32_t kDisplayFrameFirst = 0x7c0802a6;
    static constexpr uint32_t kSetSwapInterval = 0x028fad2c;
    // The vblanks a flip takes, the title's two become one.
    static constexpr uint32_t kSwapInterval = 1;
    // What the title asks for, named so a report can say what was replaced.
    static constexpr uint32_t kTitleSwapInterval = 2;
    // Fields of the display object the report reads: its phase, the interval
    // it was given, the flags the frame function toggles, and the frame counter
    // it advances. Read only, never written.
    static constexpr uint32_t kPhaseOffset = 0x28;
    static constexpr uint32_t kIntervalOffset = 0x50;
    static constexpr uint32_t kFlagsOffset = 0x74;
    static constexpr uint32_t kCounterOffset = 0x78;

    // The fork's seams, injected so this is testable without a guest.
    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe);
    using AllocateCode = uint32_t (*)(uint32_t sizeInBytes);
    // A word in the guest's own order, which is not the host's: a byte copy
    // across this boundary yields an address with its halves exchanged.
    using WriteWord = bool (*)(uint32_t guestAddress, uint32_t value);
    using ReadWord = bool (*)(uint32_t guestAddress, uint32_t& value);

    WindWakerPaint(Register registerProbe, AllocateCode allocateCode, WriteWord writeWord,
                   ReadWord readWord);

    // Registers the frame probe, before the title is linked.
    void install();

    // What the stand-in does, kept separable so a failure says which part.
    enum class Mode : uint8_t {
        // The stand-in paints once and branches back: the redirect itself,
        // with nothing added. A run where this does not hold the title's rate
        // says the redirect is at fault, not the second paint.
        PassThrough = 1,
        // Two paints, the title's own two vblanks a flip.
        Twice = 2,
        // Two paints, and one vblank a flip.
        TwiceAtSixty = 3,
    };
    // The name a refusal or a report uses for a mode.
    static std::string_view modeName(Mode mode);
    // The mode a number names, or nothing when it names none.
    static std::optional<Mode> modeFrom(long long number);

    // Puts the stand-in in and points the vtable slot at it. Empty on success,
    // otherwise the refusal, naming what it found instead of the frame.
    std::string enable(Mode mode);

    // Puts the title's own frame pointer back. Empty on success, otherwise
    // the refusal.
    std::string disable();

    // What the mod is doing, and what it has seen: paints counted, the
    // display object and its fields, the vtable slot's present value, and the
    // block's address.
    std::string json() const;

    // Paints counted, across every display thread that has run. A run where
    // this never moved is distinguishable from one where it did.
    uint64_t paints() const {
        return m_paints;
    }

    // The stand-in's words for a block at `blockAddress`, or nothing when a
    // branch in it could not reach: the title's loop body once or twice, with
    // or without the call that asks for one vblank a flip, then a branch back
    // to the top of the loop.
    static std::optional<std::vector<uint32_t>> payload(uint32_t blockAddress, Mode mode);

    // A PC-relative branch to `to`, standing at `from`, with the link register
    // written when `link` is set. These two are the only words in the payload
    // worked out rather than lifted, because their displacements depend on
    // where the block lands. Both are refused past a relative branch's reach
    // rather than written wrong.
    static uint32_t branchTo(uint32_t from, uint32_t to, bool link);
    // True when a branch at `from` can reach `to`.
    static bool withinReach(uint32_t from, uint32_t to);

  private:
    // disable()'s body, for enable() to call with the lock it holds.
    std::string disableLocked();

    // What the frame probe reported about itself, for the report to carry.
    std::string probeName() const;

    // The display object's own fields as a JSON object of its own.
    std::string displayFields() const;

    class Frame final : public GuestCallProbes::Probe {
      public:
        explicit Frame(std::atomic<uint64_t>& paints) : m_paints(paints) {
        }

        void OnInstall(GuestCallProbes::Installation installation) override;
        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;
        mutable std::mutex mutex;
        std::optional<GuestCallProbes::Installation> installation;
        uint32_t display = 0;

      private:
        std::atomic<uint64_t>& m_paints;
    };

    Register m_register;
    AllocateCode m_allocateCode;
    WriteWord m_writeWord;
    ReadWord m_readWord;
    std::atomic<uint64_t> m_paints{0};
    Frame m_frame;
    mutable std::mutex m_mutex;
    uint32_t m_block = 0;
    uint32_t m_original = 0;
    bool m_installed = false;
    Mode m_mode{Mode::TwiceAtSixty};
    std::string m_refusal;
};

} // namespace wiiuport::title
