#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
    //
    // An address out of the image is not the same as the one the title
    // installed, and patching the wrong vtable is a change to an object whose
    // slot 0xcc means something else entirely. So the vtable the running
    // display actually holds, at `display+0x24`, is read and has to be this one.
    static constexpr uint32_t kDisplayVTable = 0x10004e88;
    static constexpr uint32_t kFrameSlot = 0xcc;
    // Where the display object keeps its vtable.
    static constexpr uint32_t kVTableOffset = 0x24;
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
    // The flag says whether the probe keeps the entry. Every registration here is
    // a standing one -- each exists to count calls for the whole run -- so each
    // passes true. The parameter is here so that one `GuestCallProbes::Register`
    // address fits every seam the product hands it to.
    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry);
    using AllocateCode = uint32_t (*)(uint32_t sizeInBytes);
    // A word in the guest's own order, which is not the host's: a byte copy
    // across this boundary yields an address with its halves exchanged.
    using WriteWord = bool (*)(uint32_t guestAddress, uint32_t value);
    using ReadWord = bool (*)(uint32_t guestAddress, uint32_t& value);
    // The emulator's flip pacing, and what it was. Returns the value in force
    // after the call, so a refusal is visible as an unchanged one.
    using SetSwapInterval = uint32_t (*)(uint32_t vblanksPerFlip);
    using SwapInterval = uint32_t (*)();

    WindWakerPaint(Register registerProbe, AllocateCode allocateCode, WriteWord writeWord,
                   ReadWord readWord, SetSwapInterval setSwapInterval, SwapInterval swapInterval);

    // Registers the frame probe. Everything that changes the guest is then a
    // single word: the vtable slot. The stand-in's memory is taken when the
    // fork reports the probe installed, which is once the title's modules are
    // linked and the emulator's memory is up -- not here, which is before
    // either, and asking the loader's arena for guest memory that early takes
    // the product down before it has printed a line.
    void install();

    // What the stand-in does, kept separable so a failure says which part.
    enum class Mode : uint8_t {
        // One paint, reached by a plain branch at the frame, then back to the
        // loop. The redirect itself, with nothing added: a run where this does
        // not hold the title's rate says the redirect is at fault, not the
        // second paint.
        PassThrough = 1,
        // Two paints, the title's own two vblanks a flip.
        Twice = 2,
        // Two paints, and one vblank a flip.
        TwiceAtSixty = 3,
        // The variant that re-reads the frame from the title's vtable and goes
        // through the count register, which the other modes deliberately do not
        // do. It does not run, and it is kept because it is the falsifier for
        // that choice: same block, same words, same one rewritten word of
        // vtable, and the only difference from PassThrough is `mtctr`/`bctr`
        // against `b`. A payload that re-derives the frame every pass is a
        // nicety; one that paints nothing is not, and this is what says so.
        IndirectOnce = 4,
        // Not a paint count but a check: the title's own record of what it asked
        // for at init, at `display+0x50`, set to one, and nothing else changed.
        // That field is passed to `GX2SetSwapInterval` once during initialisation
        // and is read afterwards only to decide whether to wait for the flip, so
        // writing it may turn out to change nothing at all -- in which case the
        // rate follows from the call and not from the field, which is worth
        // knowing before anyone builds a mechanism on the field. A run of this
        // mode answers that, and a run of TwiceAtSixty answers the other half.
        IntervalField = 5,
        // One paint, at one vblank a flip: the game's own `GX2SetSwapInterval`
        // called with one ahead of it, and the frame reached by a plain branch
        // so that its return goes to the title's loop rather than back into the
        // stand-in's memory. This is the rate the mechanism is for, and it is a
        // different claim from painting twice: the flip is what paces the loop,
        // so one vblank a flip is what doubles the picture, and the second
        // paint is what will carry the blend.
        OneAtSixty = 6,
        // The control for how a stand-in is *reached*. Every other mode is
        // reached through the vtable: the display thread calls the frame, the
        // call goes through the vtable's slot, and the slot holds the stand-in's
        // address -- an indirect call the CPU resolves at the call. This one is
        // reached by writing a direct branch at the frame's entry instead, one
        // word in the title's own code, which is exactly how the logic gate
        // reaches its own block and exactly the thing that does not work there.
        //
        // So this mode is a falsifier with a known-good control in the same
        // mechanism: the payload, the interval and the rate are all unchanged, and
        // the only difference is the kind of branch. If the picture still reaches
        // sixty, a direct branch into the arena runs and the gate's difficulty is
        // somewhere else; if it does not, a direct branch out of recompiled code
        // into the loader's arena is the fault, and it is the emulator's.
        BranchEntry = 7,
    };
    // The name a refusal or a report uses for a mode.
    static std::string_view modeName(Mode mode);
    // The mode a number names, or nothing when it names none.
    static std::optional<Mode> modeFrom(long long number);

    // Puts the stand-in in and points the vtable slot at it. Empty on success,
    // otherwise the refusal, naming what it found instead of the frame.
    std::string enable(Mode mode);

    // Why the memory is not there, for a report: asked before anything runs,
    // so a refusal names the cause instead of leaving a blank block.
    const std::string& reservationRefusal() const {
        return m_reservationRefusal;
    }

    // Takes the stand-in's memory, at the moment the fork says the title is
    // linked. Called by the frame probe; public so the reservation is one
    // named thing rather than a lambda the probe holds.
    void reserve(std::string& refusal);

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
    // branch in it could not reach: one or two direct calls at the frame, with
    // or without the game's own call that asks for one vblank a flip, then an
    // absolute branch back to the top of the display thread's loop.
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

    // One reading of the display object: which vtable it holds, and its fields.
    struct DisplayFacts {
        uint32_t display = 0;
        uint32_t vtable = 0;
        std::string fields = "{}";
    };

    DisplayFacts displayFacts() const;

    class Frame final : public GuestCallProbes::Probe {
      public:
        explicit Frame(std::atomic<uint64_t>& paints) : m_paints(paints) {
        }

        // Also where the stand-in's memory is reserved, through the reservation
        // its owner hands it -- an enclosing class has no access to a nested
        // class's private members, so it is set rather than assigned.
        void OnInstall(GuestCallProbes::Installation installation) override;

        void setReservation(std::function<void(std::string&)> reserve) {
            m_reserve = std::move(reserve);
        }

        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;
        mutable std::mutex mutex;
        std::optional<GuestCallProbes::Installation> installation;
        uint32_t display = 0;

      private:
        std::atomic<uint64_t>& m_paints;
        // Called once, at link time, to take the stand-in's memory.
        std::function<void(std::string&)> m_reserve;
    };

    Register m_register;
    AllocateCode m_allocateCode;
    WriteWord m_writeWord;
    ReadWord m_readWord;
    SetSwapInterval m_setSwapInterval;
    SwapInterval m_swapInterval;
    // What the emulator's pacing was before this mod touched it.
    uint32_t m_savedPacing = 0;
    bool m_wrotePacing = false;
    std::atomic<uint64_t> m_paints{0};
    Frame m_frame;
    mutable std::mutex m_mutex;
    uint32_t m_block = 0;
    uint32_t m_original = 0;
    // The word actually written, which is the live vtable's slot and not
    // necessarily the one out of the image.
    uint32_t m_patched = 0;
    // Whether the patched word is the live vtable's slot. It is for every mode
    // but the branch-entry one, which writes a word in the title's own code, and a
    // report or a refusal that called that a slot would name a place where
    // nothing was written.
    bool m_patchedIsSlot = false;
    // The vtable the display was found to hold, reported whatever was patched.
    uint32_t m_vtableFound = 0;
    // The display's own interval field, when this mod changed it, and what it
    // was: the title's state, put back on the way out.
    uint32_t m_savedInterval = 0;
    bool m_wroteInterval = false;
    std::string m_reservationRefusal;
    bool m_installed = false;
    Mode m_mode{Mode::TwiceAtSixty};
    std::string m_refusal;
};

} // namespace wiiuport::title
