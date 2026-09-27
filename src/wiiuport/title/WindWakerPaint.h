#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <array>
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
    // **The frame's five indirect call targets, read at its entry.**
    //
    // **Every call in the frame is a `bctrl` through a target loaded out of `*(display+0x24)`** --
    // the frame does `lwz r10, 0x24(r30)` and then every target load is `0x??(r10)`: `+0xd4` at
    // 0x0274c28c, `+0xdc` at 0x0274c2a0, `+0x6c` at 0x0274c2b4, `+0xec` at 0x0274c308, and `+0xe4`
    // at 0x0274c390 -- with the argument rebuilt as `or r3, r30, r30` each time.
    //
    // **The offsets are on the pointer, not on the display.** A first version of this read them as
    // `display + 0xd4` and got four zeroes and one `0x00400000` where the title's own running
    // frame has call targets, which is what a wrong base looks like and not what a title with no
    // call targets looks like. The base is read first and the offsets applied to it.
    static constexpr uint32_t kFrameTargetBaseOffset = 0x24;
    static constexpr std::array<uint32_t, 5> kFrameCallTargetOffsets{0x6c, 0xd4, 0xdc, 0xec, 0xe4};
    // How many paints of readings the report keeps. Two, because the question is what the second
    // paint of a pass sees that the first did not, and a pair is the only shape that shows it.
    static constexpr size_t kFrameSamples = 2;
    // The fields read at every paint: the two the objective names, plus the five call targets.
    static constexpr size_t kRecentWords = 2 + kFrameCallTargetOffsets.size();

    static constexpr uint32_t kPhaseOffset = 0x28;
    static constexpr uint32_t kIntervalOffset = 0x50;
    static constexpr uint32_t kFlagsOffset = 0x74;
    static constexpr uint32_t kCounterOffset = 0x78;

    // The display fields the report names, with the offsets the title's own image puts them at. A
    // table of the four, rather than one `static constexpr` array inside the report function, which
    // the ownership rule is right to refuse: a function-local static's lifetime is not something a
    // reader can see.
    struct DisplayField {
        uint32_t offset;
        const char* name;
    };

    static constexpr std::array<DisplayField, 4> kDisplayFields{{
        {kPhaseOffset, "phase"},
        {kIntervalOffset, "interval"},
        {kFlagsOffset, "flags"},
        {kCounterOffset, "counter"},
    }};

    // The fork's seams, injected so this is testable without a guest.
    // The flag says whether the probe keeps the entry. Every registration here is
    // a standing one -- each exists to count calls for the whole run -- so each
    // passes true. The parameter is here so that one `GuestCallProbes::Register`
    // address fits every seam the product hands it to.
    // `resume` is where the call continues; zero is the instruction after the
    // entry, which is what an observer wants. It is in the signature so that one
    // `GuestCallProbes::Register` address fits every seam the product hands it to.
    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry, uint32_t resume);
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
        // Two paints where only the *first* is a call: the second is a tail branch, so the
        // frame's return goes to the title's loop exactly as it would have, and the payload
        // owns a single return path instead of two.
        //
        // **This is the discriminator for the fault that kills `Twice` and `TwiceAtSixty`.** Both
        // of those `bl` the frame twice; `OneAtSixty`, which survives, branches at it once and
        // never sets the link register. So the candidates are "the frame is not re-entrant" and
        // "the second `bl`'s link register is the problem", and this mode separates them: it
        // paints the tree twice in one pass either way, and the only difference is whether the
        // second paint returns through the payload or through the title's own loop.
        //
        // A second paint reached by a tail branch is also the more faithful shape -- the title's
        // own `bctrl` returned to its loop once per pass, and this is the stand-in doing the
        // same thing twice.
        TailTwiceAtSixty = 8,
        // Two paints, and the display pointer put back in `r3` between them.
        //
        // **This is the fix, and it came from reading the frame's own first instructions.** The
        // frame is `mfspr r0,LR; stwu r1,-0x18(r1); stw r30,...; or r30,r3,r3` -- it moves the
        // display pointer into `r30` on entry and dereferences *`r30`* for everything,
        // `display+0x74` included, while treating `r3` as a scratch register. So `r3` is not the
        // display pointer once a paint has run, and a second `bl` at the frame enters it with
        // whatever the frame last stored there -- which is the fault, and it is one fault in two
        // shapes: mode 3 puts `li r3,1` in front of the frame and never restores `r3`, and mode 2
        // lets the frame clobber `r3` itself. Either way the second paint walks a scene tree
        // through a `r30` that is not the display, and the guest ends up at an address in the
        // loader's arena.
        //
        // The word that repairs it is the title's own: `or r3, r30, r30`, which the frame uses five
        // times in its own body, once before each of its own `bctrl` calls. Lifted from 0x0274c294
        // rather than computed, because a hand-derived `or` encoding matched nothing in nine
        // megabytes of PowerPC and a payload word nobody checked has already cost a run.
        RestoreDisplayTwice = 9,
        // Two paints with the display's own per-pass flag held at the value the first one saw.
        //
        // **This is the re-entrancy, named with addresses.** The frame reads `display+0x74` at
        // 0x0274c2c4, branches three ways on bits 0 and 31 of it, and **skips a virtual `bctrl` and
        // a call to 0x0274c038** at 0x0274c300 when both are set. It then *writes* the field at
        // 0x0274c38c. So the first paint leaves the second one on a different path, and the second
        // one's near-null dereference at guest address 0x198 is what that path does with a value
        // the other path had set.
        //
        // The payload saves the field before the first paint and puts it back before the second, so
        // both paints run the path the first one chose. **Every word is the frame's own,
        // verbatim**: the load is 0x0274c2c4 and the store is 0x0274c38c, both with the frame's own
        // `r30` base.
        SamePhaseTwice = 10,
        // **The objective's payload, word for word.** Eleven words -- its group of five twice, then
        // a branch back to the title's loop:
        //
        //     lwzu r3,0x24(r30)   lwz r12,0xcc(r0)   mtspr CTR,r12   or r31,r3,r3   bctrl
        //
        // **This is the only mode that reaches the frame the way the title does**: by loading it
        // out of the vtable and calling it indirectly. Every other mode reaches it by a literal
        // `bl` and never touches the vtable from inside the payload.
        //
        // It matters because the measured fault is a guest load at `0x198`, and `0x198` is
        // `0xcc + 0xcc` -- the second word of this payload with a small number where the vtable
        // pointer belongs. The other modes cannot produce that load at all, so a run of this one
        // either works, which settles conditions 3 and 4, or fails differently, which says the
        // difference between the two shapes is the whole of it.
        //
        // The eleventh word is the objective's `4e800020`, lifted as a form: a `b` whose
        // displacement reaches the title's loop from wherever the loader's arena put this block. It
        // is the one word here that is not verbatim, because a fixed displacement can only reach
        // one address.
        ObjectivePayload = 11,
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

    // The counter itself, for a consumer that has to tell one frame from another and not
    // merely count them. A count is no use to anything that has to schedule its own reads.
    const std::atomic<uint64_t>& paintCounter() const {
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
        // `readWord` is the owner's seam, handed in rather than reached for: a
        // nested class has no access to its enclosing class's members.
        Frame(std::atomic<uint64_t>& paints, ReadWord readWord)
            : m_paints(paints), m_readWord(readWord) {
        }

        // Also where the stand-in's memory is reserved, through the reservation
        // its owner hands it -- an enclosing class has no access to a nested
        // class's private members, so it is set rather than assigned.
        void OnInstall(GuestCallProbes::Installation installation) override;

        void setReservation(std::function<void(std::string&)> reserve) {
            m_reserve = std::move(reserve);
        }

        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;

        // `display+0x74` and `display+0x28`, sampled at every paint. The
        // frame's `if (flags & 1) flags ^= 2` toggle is a per-paint event, and a
        // report that reads the field on request sees one value rather than the
        // sequence, which is the difference between "the toggle is not happening"
        // and "the toggle is not visible in one sample".
        std::array<uint32_t, 2> recent{0, 0};
        // The five call targets as read at the last paint, and the last two paints' readings of
        // everything: the two fields the objective names and the five targets, one row per paint.
        std::array<uint32_t, kFrameCallTargetOffsets.size()> callTargets{0};
        uint32_t callTargetBase = 0;
        std::array<std::array<uint32_t, kRecentWords>, kFrameSamples> samples{};
        // False until there are two paints to compare, because one paint's reading is not a pair
        // and a report that showed it as one would be showing nothing.
        bool samplesValid = false;
        // Held for the whole time the display thread is inside the frame, and
        // taken by whoever rewrites the words that route it here.
        //
        // This is what makes arming and disarming safe. The stand-in is reached
        // through a word in a vtable slot, and a display thread already inside it
        // is running code that the next word written may invalidate -- the
        // recompiled function covering the stand-in's block is deleted, and the
        // core is still in it. Measured: a segmentation fault inside recompiled
        // code one millisecond after the patch was armed, with the frame's own
        // address 0x0274c268 in the stack, and nothing in the title's logs.
        //
        // The lock is the probe's own, already taken on every call, so a patch
        // that takes it waits for the display thread to leave rather than
        // invalidating the code under it. It is not a lock the guest takes: the
        // guest does not know it exists, and a display thread that is mid-frame
        // finishes its frame and releases it in the ordinary way.
        mutable std::mutex mutex;
        std::optional<GuestCallProbes::Installation> installation;
        uint32_t display = 0;

      private:
        std::atomic<uint64_t>& m_paints;
        ReadWord m_readWord;
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
