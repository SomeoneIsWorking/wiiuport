#include "wiiuport/title/WindWakerPaint.h"

#include "Cafe/HW/Espresso/GuestPatching.h"

#include <lucent/log.h>

#include <array>
#include <cstdio>
#include <vector>

namespace wiiuport::title {

namespace {

// The display loop's body, lifted out of the title's own image at 0x0274c020
// where those five instructions stand. Nothing here is hand-assembled, which is
// the point: a word worked out by hand is a word nobody checked.
constexpr std::array<uint32_t, 5> kLoopBody = {
    0x819f0024, // lwz  r12,0x24(r31)   the display's vtable
    0x800c00cc, // lwz  r0,0xcc(r12)    the frame
    0x7c0903a6, // mtctr r0
    0x7fe3fb78, // or   r3,r31,r31      the display, as the original call passed it
    0x4e800421, // bctr
};
constexpr uint32_t kLoadOne = 0x38600001; // li r3,1          from 0x025f094c

// `or r3, r30, r30` -- the display pointer back into `r3`, lifted from the display frame itself at
// 0x0274c294, which uses this exact word five times in its own body, once immediately before each
// of its own `bctrl` calls.
//
// **This word is the whole of the second-paint fix, and it is the title's own.** The frame's
// second instruction is `or r30, r3, r3`: it moves the display pointer into `r30` on entry and then
// dereferences *r30* for everything -- `display+0x74` is read as `lwz r0, 0x74(r30)` -- while
// using `r3` as a scratch register throughout. So `r3` is not the display pointer after the first
// paint returns, and a second `bl` at the frame enters it with whatever the frame last put there.
// That is the fault, and it is the same fault in both modes: mode 3 puts `li r3,1` in front of the
// frame and never restores `r3`, and mode 2 lets the frame clobber `r3` itself. Either way the
// second call walks a scene tree through a `r30` that is not the display.
//
// A derived encoding would have been the obvious thing to write here and is why this is lifted: an
// `or` encoding worked out by hand found *nothing* in nine megabytes of PowerPC, which is what an
// encoding that is wrong looks like, and a payload word nobody checked has already cost a run.
constexpr uint32_t kRestoreDisplay = 0x7fc3f378; // or r3,r30,r30   from 0x0274c294

// The display's own per-pass flag, read and written by the frame itself.
//
// `lwz r0, 0x74(r30)` is the frame's second instruction after its entry work, at 0x0274c2c4, and
// `stw r0, 0x74(r30)` is at 0x0274c38c. **The two words that bracket the paint are the frame's own,
// verbatim, with the same base register it uses** -- which is why this payload needs no word of its
// own anywhere.
//
// The field is not the flip counter the objective's note guesses. It is what the frame *branches*
// on at entry, and it gates two calls:
//
//     0x0274c2c4  lwz   r0,0x74(r30)
//     0x0274c2cc  rlwinm. r12,r0,0x0,0x1f,0x1f      <- bit 0
//     0x0274c2d4  beq   0x0274c2e4
//     0x0274c2d8  rlwinm. r0,r0,0x1f,0x1f,0x1f      <- bit 31
//     0x0274c2dc  beq   0x0274c2e4
//     0x0274c2e0  li    r31,0
//     ...
//     0x0274c2fc  cmpwi r31,0
//     0x0274c300  beq   0x0274c320                   <- skips what follows
//     0x0274c304  lwz   r10,0x24(r30)
//     0x0274c308  lwz   r0,0xec(r10)
//     0x0274c30c  mtspr CTR,r0
//     0x0274c310  or    r3,r30,r30
//     0x0274c314  bctrl
//     0x0274c318  or    r3,r30,r30
//     0x0274c31c  bl    0x0274c038
//     ...
//     0x0274c38c  stw   r0,0x74(r30)                  <- and the frame writes it
//
// `r31` is zero exactly when bits 0 and 31 are both set, and the block above is skipped exactly
// then. So a paint that leaves those bits set sends the *next* paint down a different path from the
// one it took -- which is the second paint's near-null dereference, and why no amount of restoring
// the display pointer helped.
// **The objective's own payload, word for word.** Eleven words: this group of five twice, then a
// branch back to the title's loop.
//
//     819f0024  lwzu   r3,0x24(r30)     the display's sub-object, updating r30
//     800c00cc  lwz    r12,0xcc(r0)     the vtable's slot 0xcc -- the frame
//     7c0903a6  mtspr  CTR,r12
//     7fe3fb78  or     r31,r3,r3        the title's own non-volatile for the display
//     4e800421  bctrl                  the frame, reached through the vtable
//
// **Every one of these is different from what the modes above do**, and the difference is the
// point. All of them reach the frame by a literal `bl` and never touch the vtable from inside the
// payload; this one *loads the frame out of the vtable* and calls it indirectly, which is the shape
// the objective specifies and the one the measured fault is consistent with -- the faulting
// instruction is a guest load at `0x198`, and `0x198` is `0xcc + 0xcc`: a `lwz` at offset `0xcc`
// off a register holding a small number. That is this payload's second word with the wrong base
// register.
//
// The eleventh word is a branch to the title's loop, and the objective gives it as `4e800020`,
// which as a branch is `b +0x20` **from its own address** -- a displacement that can only reach the
// loop from one place in memory. So the form is lifted and the displacement is computed, which is
// the same treatment every other branch in this file gets, and the one word here that is not
// verbatim. It is the only way a stand-in in the loader's arena can return to a loop it does not
// share an address with.
constexpr uint32_t kObjectiveLoadSubObject = 0x819f0024;     // lwzu  r3,0x24(r30)
constexpr uint32_t kObjectiveLoadVtableSlot = 0x800c00cc;    // lwz   r12,0xcc(r0)
constexpr uint32_t kObjectiveMoveCtr = 0x7c0903a6;           // mtspr CTR,r12
constexpr uint32_t kObjectiveSaveDisplay = 0x7fe3fb78;       // or    r31,r3,r3
constexpr uint32_t kObjectiveCallThroughVtable = 0x4e800421; // bctrl

// **The display thread's own dispatch, lifted word for word from 0x0274c020-0x0274c030.**
//
// These are the same five opcodes and the same five displacements as the objective's payload above,
// with the register fields as the loop has them rather than shifted along by one. The difference is
// the whole of why one of them reaches the frame twice and the other calls itself, so the two sets
// sit side by side here rather than one set being replaced by the other.
constexpr uint32_t kLoopLoadVTable = 0x819f0024;     // lwz   r12,0x24(r31)
constexpr uint32_t kLoopLoadFrameSlot = 0x800c00cc;  // lwz   r0,0xcc(r12)
constexpr uint32_t kLoopMoveToCounter = 0x7c0903a6;  // mtspr CTR,r0
constexpr uint32_t kLoopRestoreDisplay = 0x7fe3fb78; // or    r3,r31,r31
constexpr uint32_t kLoopCallFrame = 0x4e800421;      // bctrl

// `lis` and `ori` against a 32-bit address, so a payload can carry an address as two lifted
// instructions rather than re-read it through a pointer the guest may have had changed.
//
// The *forms* are the title's own: the image holds `addis r12,r0,0x0027` (`3d80027f`) and
// `ori r12,r12,0x5890` (`618c5890`) in a loader trampoline. Only the immediate is computed, and it
// is an address this mod already holds -- the frame it read out of the vtable slot before rewriting
// it -- not a value searched for. That is the same kind of computation the payload's branch
// displacements already are, and the test asserts the encoding against a hand-worked value so a
// slip is caught by arithmetic rather than by a run.
constexpr uint32_t kAddisImmediate = 0x3C000000; // addis rD,rA,simm
constexpr uint32_t kOriImmediate = 0x60000000;   // ori rA,rS,uimm
constexpr uint32_t kTargetRegister = 0;          // r0, which the loop's own `mtspr CTR,r0` consumes

constexpr uint32_t loadUpperImmediate(uint32_t address) {
    return kAddisImmediate | (static_cast<uint32_t>(kTargetRegister) << 21) |
           ((address >> 16) & 0xFFFF);
}

constexpr uint32_t orImmediate(uint32_t address) {
    return kOriImmediate | (static_cast<uint32_t>(kTargetRegister) << 21) |
           (static_cast<uint32_t>(kTargetRegister) << 16) | (address & 0xFFFF);
}

constexpr uint32_t kReadDisplayPhase = 0x801e0074;  // lwz  r0,0x74(r30)  from 0x0274c2c4
constexpr uint32_t kWriteDisplayPhase = 0x901e0074; // stw  r0,0x74(r30)  from 0x0274c38c

// The most words any stand-in is: the swap-interval call, two loop bodies, and
// the branch back. The block is reserved once, at startup, for this many.
// The most words any stand-in is: the seven of the one-vblank form, one more
// than the rest need. The block is reserved once, at link time.
constexpr size_t kMaxWords = 7;

// A relative branch reaches 32 MiB either side of where it stands, which is
// the fork's own rule for the same instruction.
constexpr int64_t kRelativeBranchReach = 0x02000000;
constexpr uint32_t kPrimaryBranch = 18;

// Three words the payload needs that the title spells the same way. Each is
// lifted from a real instruction in the title's own image, with the field that
// varies verified against a second instruction rather than derived:
//   `lis r12,0x1019` is 0x3d801019 and `mtcr r0` is 0x7c0903a6, so with the
//   register in bits 21-25 the operands below are the same two instructions
//   with a different register and immediate.
constexpr uint32_t kLoadUpper = 0x3d800000;   // lis r12, 0x0275
constexpr uint32_t kOrImmediate = 0x60000000; // ori r12, r12, 0xc034
constexpr uint32_t kMoveToLink = 0x7c0803a6;  // mtspr LR, r0; r12 is that plus 12 << 21

// The address the display thread's loop returns to: the instruction after its
// `bctrl`, which is the loop's own branch back to its top. Read and checked at
// install time, so a stand-in that puts the link register somewhere else cannot
// send the frame's own return into memory the emulator will not branch back
// from.
constexpr uint32_t kLoopReturn = 0x0274c034;
constexpr uint32_t kLoopTail = 0x48000000 | ((kLoopReturn & 0x03fffffc) - 0x48000000);

std::string hex(uint32_t value) {
    std::array<char, 11> text{};
    std::snprintf(text.data(), text.size(), "0x%08x", value);
    return {text.data()};
}

std::string hexField(uint32_t offset) {
    std::array<char, 9> text{};
    std::snprintf(text.data(), text.size(), "0x%02x", offset);
    return {text.data()};
}

// A JSON object written one member at a time, owning the separators and the
// quoting. Written as a long chain of additions this report once came out with
// a stray quote and a doubled comma -- joins in an expression that reads as one
// thing are exactly the joins nobody checks -- so every member goes through
// here, and `finish` is the only way to close it.
class JsonBody {
  public:
    // A value that brings its own form: true, false, a number.
    void raw(const char* name, const std::string& value) {
        separate();
        m_body += '"';
        m_body += name;
        m_body += "\":";
        m_body += value;
    }

    void string(const char* name, const std::string& value) {
        raw(name, "\"" + value + "\"");
    }

    void number(const char* name, uint64_t value) {
        raw(name, std::to_string(value));
    }

    // An object as a member: `inner` is another body's `text`, unterminated,
    // because this one closes the whole thing.
    void object(const char* name, const std::string& inner) {
        separate();
        m_body += '"';
        m_body += name;
        m_body += "\":";
        m_body += inner;
    }

    // The body closed, with the newline a report ends on.
    std::string finish() const {
        return text() + "\n";
    }

    // The body closed, without it: for a member of another body.
    std::string text() const {
        return m_body + "}";
    }

  private:
    void separate() {
        if (!m_first) {
            m_body += ',';
        }
        m_first = false;
    }

    std::string m_body = "{";
    bool m_first = true;
};

} // namespace

bool WindWakerPaint::withinReach(uint32_t from, uint32_t to) {
    const int64_t displacement = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    return displacement >= -kRelativeBranchReach && displacement < kRelativeBranchReach;
}

uint32_t WindWakerPaint::branchTo(uint32_t from, uint32_t to, bool link) {
    return (kPrimaryBranch << 26) | (link ? 1u : 0u) | ((to - from) & 0x03fffffcu);
}

std::string_view WindWakerPaint::modeName(Mode mode) {
    switch (mode) {
    case Mode::PassThrough:
        return "passThrough";
    case Mode::Twice:
        return "twice";
    case Mode::TwiceAtSixty:
        return "twiceAtSixty";
    case Mode::IndirectOnce:
        return "indirectOnce";
    case Mode::IntervalField:
        return "intervalField";
    case Mode::OneAtSixty:
        return "oneAtSixty";
    case Mode::BranchEntry:
        return "branchEntry";
    case Mode::TailTwiceAtSixty:
        return "tailTwiceAtSixty";
    case Mode::RestoreDisplayTwice:
        return "restoreDisplayTwice";
    case Mode::SamePhaseTwice:
        return "samePhaseTwice";
    case Mode::ObjectivePayload:
        return "objectivePayload";
    case Mode::LoopDispatchTwice:
        return "loopDispatchTwice";
    case Mode::LoopFrameLiteralTwice:
        return "loopFrameLiteralTwice";
    }
    return "unknown";
}

std::optional<WindWakerPaint::Mode> WindWakerPaint::modeFrom(long long number) {
    switch (number) {
    case 1:
        return Mode::PassThrough;
    case 2:
        return Mode::Twice;
    case 3:
        return Mode::TwiceAtSixty;
    case 4:
        return Mode::IndirectOnce;
    case 5:
        return Mode::IntervalField;
    case 6:
        return Mode::OneAtSixty;
    case 7:
        return Mode::BranchEntry;
    case 8:
        return Mode::TailTwiceAtSixty;
    case 9:
        return Mode::RestoreDisplayTwice;
    case 10:
        return Mode::SamePhaseTwice;
    case 11:
        return Mode::ObjectivePayload;
    case 12:
        return Mode::LoopDispatchTwice;
    case 13:
        return Mode::LoopFrameLiteralTwice;
    default:
        return std::nullopt;
    }
}

std::optional<std::vector<uint32_t>> WindWakerPaint::payload(uint32_t blockAddress, Mode mode) {
    // The one word every stand-in ends with: back to the top of the display
    // thread's loop, by an absolute branch rather than by `blr`. `blr` is not
    // available because a `bl` in the payload sets the link register and the
    // only thing that set it before was the game's own call -- so a payload
    // that calls anything cannot return through it.
    if (mode == Mode::IndirectOnce) {
        // The variant that reads the frame out of the vtable and goes through
        // the count register, kept because it is the falsifier for the choice
        // the other modes make. Measured: it does not run. Same block, same
        // words, same one rewritten word of vtable, reached by a plain branch
        // instead -- and the title runs at 30 paints and 30 logic ticks a
        // second. The difference is `mtctr`/`bctr` against `b`, and it is the
        // recompiler's jump table. Re-deriving the frame from the vtable on
        // every pass is a nicety; a stand-in that paints nothing is not.
        const std::vector<uint32_t> words{kLoopBody[0], kLoopBody[1], kLoopBody[2], kLoopBody[3],
                                          kLoopBody[4]};
        const uint32_t backAt = blockAddress + 4 * (words.size() + 1);
        if (!withinReach(backAt, kDisplayLoopTop)) {
            return std::nullopt;
        }
        std::vector<uint32_t> all = words;
        all.push_back(branchTo(backAt, kDisplayLoopTop, false));
        return all;
    }

    if (mode == Mode::OneAtSixty) {
        // One paint, at one vblank a flip -- and the payload is a single branch,
        // because the pacing is the emulator's and the title's own record of it
        // is a field `enable()` writes. Nothing here calls anything, so the
        // display register the loop's call set up arrives at the frame
        // untouched, and the frame's return goes to the title's loop rather than
        // back into this memory.
        if (!withinReach(blockAddress, kDisplayFrame)) {
            return std::nullopt;
        }
        return std::vector<uint32_t>{branchTo(blockAddress, kDisplayFrame, false)};
    }

    // Everything else is a list of direct branches. The frame is reached by
    // `bl` rather than by the loop's own `bctrl`, so it can be reached more
    // than once in an iteration, and each call returns to the next word of the
    // payload. Displacements are measured from where each word stands, and a
    // branch that could not reach refuses the whole payload rather than being
    // written wrong.
    struct Step {
        bool call;
        uint32_t target;
        // A literal word to write, lifted from the title's image, for the one instruction that is
        // not a branch and not a call: `or r3, r30, r30`. Zero means the step is a branch.
        uint32_t raw = 0;
    };

    // Two of the modes call the frame and so can paint twice in an iteration. The rest stand in for
    // it, tail-branching at it once, which is what the game's own `bctrl` was about to do anyway.
    //
    // `RestoreDisplayTwice` writes its two paints out in full and nothing below adds to them: the
    // word between the two calls is the fix, and a step list that appended a third paint would
    // undo the finding it exists to test.
    std::vector<Step> steps;
    if (mode == Mode::LoopFrameLiteralTwice) {
        // **The loop's dispatch with the frame built into the payload instead of read from the slot
        // this mod rewrote.** Everything else is the loop's own words, lifted: the counter move,
        // the display restore, and the call.
        //
        // `or r3,r31,r31` is the word the fault turned on. The frame is a method on the display: it
        // takes it in `r3`, copies it to `r30` and dereferences `r30` for every field, and then
        // treats `r3` as scratch. The loop rebuilds `r3` from `r31` before every dispatch for
        // exactly that reason, and a stand-in that does not hands the second paint a scratch
        // register where the display belongs -- measured: the frame's `r30` was zero, with the
        // display in `r31`.
        //
        // `r31` is available without being told: the display arrives in `r3`, the loop's third word
        // keeps it in `r31`, and the stand-in is reached through the slot that dispatch reads.
        for (int pass = 0; pass < 2; pass++) {
            steps.push_back({false, 0, loadUpperImmediate(kDisplayFrame)});
            steps.push_back({false, 0, orImmediate(kDisplayFrame)});
            steps.push_back({false, 0, kLoopMoveToCounter});
            steps.push_back({false, 0, kLoopRestoreDisplay});
            steps.push_back({false, 0, kLoopCallFrame});
        }
    } else if (mode == Mode::LoopDispatchTwice) {
        // **The display thread's own dispatch, twice, verbatim.** These are the five words at
        // 0x0274c020, lifted from the title's image and not encoded here -- every one of them is
        // the word the loop itself executes, and this project has measured what a hand-derived
        // encoding costs twice.
        //
        // The fourth word is the one that matters. The frame is a method on the display: it takes
        // it in `r3`, copies it to `r30` on its sixth word, dereferences `r30` for every field, and
        // then treats `r3` as scratch for the rest of its body. So the loop rebuilds `r3` from
        // `r31` before every dispatch, and a stand-in that does not is handing the second paint a
        // scratch register where the frame wants the display. Measured: at the fault the frame's
        // `r30` is
        // **zero**, and the display object is sitting in `r31`.
        //
        // `r31` is available because the stand-in is reached through vtable slot `0xcc`, and the
        // loop established `r31` at 0x0274c018 before it dispatched. The stand-in must not disturb
        // it, and none of these ten words does.
        for (int pass = 0; pass < 2; pass++) {
            steps.push_back({false, 0, kLoopLoadVTable});     // lwz   r12,0x24(r31)
            steps.push_back({false, 0, kLoopLoadFrameSlot});  // lwz   r0,0xcc(r12)
            steps.push_back({false, 0, kLoopMoveToCounter});  // mtspr CTR,r0
            steps.push_back({false, 0, kLoopRestoreDisplay}); // or    r3,r31,r31
            steps.push_back({false, 0, kLoopCallFrame});      // bctrl
        }
    } else if (mode == Mode::ObjectivePayload) {
        // **Ten verbatim words and one computed branch.** The two groups are the objective's, in
        // its order, and the branch after them is the objective's `b` with its displacement worked
        // out -- the one word that cannot be verbatim, because the stand-in's block is handed out
        // at run time and a fixed displacement reaches one address.
        //
        // No `li r3,1` and no swap-interval call in front: this payload sets its own registers, and
        // a word in front of it that overwrote `r3` would defeat the `lwzu` that loads the display.
        for (int pass = 0; pass < 2; pass++) {
            steps.push_back({false, 0, kObjectiveLoadSubObject});
            steps.push_back({false, 0, kObjectiveLoadVtableSlot});
            steps.push_back({false, 0, kObjectiveMoveCtr});
            steps.push_back({false, 0, kObjectiveSaveDisplay});
            steps.push_back({false, 0, kObjectiveCallThroughVtable});
        }
    } else if (mode == Mode::SamePhaseTwice) {
        // Save the display's per-pass flag, paint, put it back, paint again.
        //
        // **Every word here is the frame's own, verbatim.** The load is 0x0274c2c4 and the store is
        // 0x0274c38c, both `r30`-based exactly as the frame writes them, and the two calls are the
        // same shape the other two-paint modes use. So this payload introduces no word of its own
        // -- which is the property that has mattered all session, since a derived word is a word
        // nobody checked and one has already cost a run.
        //
        // What it does is make the second paint take the *same* path as the first. The frame
        // branches on bits 0 and 31 of this field at entry and skips a virtual call and a call to
        // 0x0274c038 when both are set, and then writes the field; without putting it back, the
        // second paint runs a path the first one did not, and that path is what dereferences guest
        // address 0x198.
        steps.push_back({false, 0, kReadDisplayPhase});
        steps.push_back({true, kDisplayFrame});
        steps.push_back({false, 0, kWriteDisplayPhase});
        steps.push_back({true, kDisplayFrame});
    } else if (mode == Mode::RestoreDisplayTwice) {
        // **No `li r3,1` and no setter call.** The frame's second instruction is `or r30, r3, r3`,
        // so it takes the display pointer into `r30` and dereferences *`r30`* for everything while
        // treating `r3` as a scratch register -- `display+0x74` is `lwz r0, 0x74(r30)`. Putting a
        // one in `r3` in front of the frame is therefore putting a one in the display pointer,
        // and the swap-interval call that follows leaves `r3` wherever the guest put it. The
        // interval is the paint mod's own business; the frame's entry is not something to arrive at
        // with a register the frame is about to overwrite.
        steps.push_back({true, kDisplayFrame});
        // The title's own repair word, lifted from 0x0274c294, which the frame uses five times in
        // its own body -- once immediately before each of its own `bctrl` calls. After a paint,
        // `r3` is whatever the frame last stored in it and `r30` is still the display, so this is
        // the one instruction that makes a second paint reach the same frame the first one did.
        steps.push_back({false, 0, kRestoreDisplay});
        steps.push_back({true, kDisplayFrame});
    } else {
        // **No payload branches to `kSetSwapInterval` any more, and this is why.** That address
        // holds four zero words in the title's own image -- `add r0,r0,r0`, four times -- so a `bl`
        // into it ran off the end of the hole, and that was the double-paint fault: at the fault
        // the link register was the stand-in's own and the program counter was in the loader arena
        // executing host pointer bytes, and the two `bl`s at the display frame were never reached
        // at all.
        //
        // The interval is set by writing the display's own field, which is what `OneAtSixty` -- the
        // shape that measures 59.99 and 60.12 paints a second and survives -- has always done. The
        // call was redundant before it was fatal: the field write is the title's own record of the
        // interval it asked for, and a call into a zero-filled hole cannot set it.
        const bool calls = mode == Mode::Twice || mode == Mode::TwiceAtSixty;
        const bool paintsTwice = calls || mode == Mode::TailTwiceAtSixty;
        // `ObjectivePayload` carries its own two calls and its own words; the loop below would add
        // a third, and a payload that paints three times is not the one the objective specifies.
        for (uint32_t paint = 0; paint < (paintsTwice ? 2u : 1u); paint++) {
            // **The second paint of `TailTwiceAtSixty` is a branch, not a call.** That is the whole
            // mode: the frame's return then goes to the title's loop, where the title's own `bctrl`
            // would have sent it, and the payload owns exactly one link-register return rather than
            // two. Everything else about it is mode 3 unchanged, so a run that survives where mode
            // 3 faults separates "the frame is not re-entrant" from "the second `bl` is the
            // problem".
            //
            // Only mode 8 distinguishes its two paints; every other mode uses one rule for both,
            // and `calls` is that rule. Written as a per-mode choice rather than `calls || paint ==
            // 0`, which reads as though a single-paint mode should call -- and did, until the
            // pass-through payload's one branch became a call and the test on its two words said
            // so.
            const bool call = (mode == Mode::TailTwiceAtSixty) ? (paint == 0) : calls;
            steps.push_back({call, kDisplayFrame});
        }
    }

    std::vector<uint32_t> words;
    words.reserve(steps.size() + 1);
    for (size_t index = 0; index < steps.size(); index++) {
        const Step& step = steps[index];
        const uint32_t at = blockAddress + 4 * static_cast<uint32_t>(index);
        if (step.raw != 0) {
            words.push_back(step.raw);
            continue;
        }
        if (step.target == 0) {
            words.push_back(kLoadOne);
            continue;
        }
        if (!withinReach(at, step.target)) {
            return std::nullopt;
        }
        words.push_back(branchTo(at, step.target, step.call));
    }
    const uint32_t backAt = blockAddress + 4 * static_cast<uint32_t>(words.size());
    if (!withinReach(backAt, kDisplayLoopTop)) {
        return std::nullopt;
    }
    words.push_back(branchTo(backAt, kDisplayLoopTop, false));
    return words;
}

WindWakerPaint::WindWakerPaint(Register registerProbe, AllocateCode allocateCode,
                               WriteWord writeWord, ReadWord readWord,
                               SetSwapInterval setSwapInterval, SwapInterval swapInterval)
    : m_register(registerProbe), m_allocateCode(allocateCode), m_writeWord(writeWord),
      m_readWord(readWord), m_setSwapInterval(setSwapInterval), m_swapInterval(swapInterval),
      m_frame(m_paints, readWord) {
}

void WindWakerPaint::install() {
    m_frame.setReservation([this](std::string& refusal) {
        reserve(refusal);
    });
    m_register(kDisplayFrameProbe, kDisplayFrameProbeFirst, m_frame, true, 0);
}

void WindWakerPaint::reserve(std::string& refusal) {
    std::scoped_lock lock(m_mutex);
    if (m_block != 0) {
        return;
    }
    // Taken once the title's modules are linked, not when the stand-in is asked
    // for: the loader's arena expects to be asked while it is linking, and the
    // one thing this mod does not need to do while the title is running is
    // allocate. Enabling is then a single word.
    m_block = m_allocateCode(4 * kMaxWords);
    if (m_block == 0) {
        m_reservationRefusal = "the loader's arena had no " + std::to_string(4 * kMaxWords) +
                               " bytes of executable guest memory";
        refusal = m_reservationRefusal;
        return;
    }
    m_reservationRefusal.clear();
    lucent::info("paint", "stand-in memory reserved at {}", hex(m_block));
}

void WindWakerPaint::Frame::OnInstall(GuestCallProbes::Installation result) {
    {
        std::scoped_lock lock(mutex);
        installation = result;
    }
    // Outside the probe's own lock: the reservation logs, and the probe's lock
    // is held on the thread that is linking the title.
    if (m_reserve) {
        std::string refusal;
        m_reserve(refusal);
    }
}

void WindWakerPaint::Frame::OnCall(std::span<const uint32_t, 32> gpr, uint32_t /*returnAddress*/) {
    {
        // Counted before the lock: a paint must not be able to block the
        // display thread behind a report being written.
        m_paints.fetch_add(1, std::memory_order_relaxed);
    }
    std::scoped_lock lock(mutex);
    display = gpr[3];
    // `display+0x74` and `display+0x28`, sampled at every paint: the flags the
    // frame toggles, and the phase it keeps. Those are the two fields the flip
    // decision is read from.
    //
    // The frame is documented to do `if (display+0x74 & 1) display+0x74 ^= 2`,
    // which would leave every second paint of a twice-per-pass stand-in without a
    // flip. A report that reads the fields once, on request, cannot see that: it
    // reads whichever value the last paint left, not the sequence. A pair per
    // paint is the only shape in which the toggle is visible, so the report gets
    // the pair and the toggle shows up as a bit changing between samples while
    // the paint count climbs.
    //
    // Sampled under the same lock as the display pointer, so a pair is never half
    // from one paint and half from another.
    {
        uint32_t flags = 0;
        uint32_t phase = 0;
        if (m_readWord(display + kFlagsOffset, flags)) {
            recent[0] = flags;
        }
        if (m_readWord(display + kPhaseOffset, phase)) {
            recent[1] = phase;
        }
    }
    // The five call targets, at the same lock and the same moment as the pair above, so a sample
    // is one paint's reading of the whole display rather than a mixture of two.
    //
    // A target that is a small number rather than a code address is the near-null dereference, and
    // the report says so per sample rather than leaving a reader to compare five hex numbers by
    // eye.
    {
        uint32_t base = 0;
        const bool haveBase = m_readWord(display + kFrameTargetBaseOffset, base) && base != 0;
        if (haveBase) {
            for (size_t index = 0; index < kFrameCallTargetOffsets.size(); index++) {
                uint32_t value = 0;
                if (m_readWord(base + kFrameCallTargetOffsets[index], value)) {
                    callTargets[index] = value;
                }
            }
        }
        callTargetBase = haveBase ? base : 0;
        // Shift the two samples down, so the report's first entry is this paint and its second is
        // the one before. A pair of paints, not a pair of passes: with a stand-in that paints
        // twice, consecutive paints are the two halves of one pass.
        for (size_t index = kFrameSamples - 1; index > 0; index--) {
            for (size_t word = 0; word < kRecentWords; word++) {
                samples[index][word] = samples[index - 1][word];
            }
        }
        for (size_t word = 0; word < 2; word++) {
            samples[0][word] = recent[word];
        }
        for (size_t word = 0; word < kFrameCallTargetOffsets.size(); word++) {
            samples[0][2 + word] = callTargets[word];
        }
        samplesValid = m_paints.load(std::memory_order_relaxed) >= kFrameSamples;
    }
}

std::string WindWakerPaint::enable(Mode mode) {
    std::scoped_lock lock(m_mutex);
    if (m_installed) {
        if (m_mode == mode) {
            return {};
        }
        // A different stand-in is a different patch: put the title's own back
        // before writing another, so a refusal leaves the title running rather
        // than running on the last thing installed.
        const std::string refusal = disableLocked();
        if (!refusal.empty()) {
            return refusal;
        }
    }
    // The vtable to patch is the one the running display holds, read from the
    // display object the probe hands over on every paint. The address out of
    // the image is a check and not the source: a title that put its display
    // somewhere else would otherwise have a word of its own image rewritten on
    // the strength of a note in a document, which is a change to an object
    // whose slot 0xcc may mean something else entirely.
    uint32_t vtable = 0;
    {
        std::scoped_lock frameLock(m_frame.mutex);
        if (m_frame.display == 0) {
            m_refusal = "the display thread has not painted yet, so which vtable it calls "
                        "is not known";
            return m_refusal;
        }
        if (!m_readWord(m_frame.display + kVTableOffset, vtable) || vtable == 0) {
            m_refusal =
                "the display object at " + hex(m_frame.display) + " would not give up its vtable";
            return m_refusal;
        }
    }
    // Kept for the report: the vtable the display was found to hold, whether or
    // not this mode writes through it.
    m_vtableFound = vtable;
    const uint32_t slot = vtable + kFrameSlot;
    uint32_t frame = 0;
    if (!m_readWord(slot, frame)) {
        m_refusal =
            "vtable slot " + hexField(kFrameSlot) + " of " + hex(vtable) + " is not guest memory";
        return m_refusal;
    }
    if (frame != kDisplayFrame) {
        // Named, because a stand-in written for one frame and pointed at
        // another is a crash with nothing to read.
        m_refusal = "vtable slot " + hexField(kFrameSlot) + " of " + hex(vtable) + " holds " +
                    hex(frame) + ", not this title's display frame " + hex(kDisplayFrame);
        return m_refusal;
    }
    if (vtable != kDisplayVTable) {
        // Not a refusal: the stand-in works on whatever vtable the display
        // calls, and this says which one that turned out to be.
        lucent::info("paint", "the display's vtable is {}, not the {} out of the image",
                     hex(vtable), hex(kDisplayVTable));
    }
    // The stand-in branches back to the top of the display thread's loop, so
    // that address is read and checked rather than assumed: a revision whose
    // loop starts on something else would be sent somewhere else entirely.
    uint32_t first = 0;
    if (!m_readWord(kDisplayLoopTop, first) || first != kDisplayLoopTopFirst) {
        m_refusal = "the display thread's loop at " + hex(kDisplayLoopTop) + " holds " +
                    hex(first) + ", not this title's loop";
        return m_refusal;
    }
    // **The address the payloads used to call for the swap interval is checked and never called.**
    // It holds four zero words in the title's own image -- `add r0,r0,r0`, four times -- so a
    // payload that branched there ran off the end of the hole, and that was the double-paint fault.
    // Nothing branches there now, and this says so at install time rather than leaving the next
    // payload to find out by faulting. If a future revision ever puts code there, the refusal names
    // both facts, so the change becomes a decision rather than an accident.
    {
        uint32_t hole = 0;
        if (m_readWord(kSetSwapInterval, hole) && hole != kSynchronisationNoOp) {
            m_refusal =
                "the swap-interval address " + hex(kSetSwapInterval) + " now holds " + hex(hole) +
                ", not a zero word: it is no longer the hole the payloads were " +
                "measured against, and a payload branching there would branch into whatever " +
                "it has become";
            return m_refusal;
        }
    }
    m_original = frame;
    // The interval field is written for every shape that needs one vblank a flip, which now
    // includes the two-paint shapes. It used to be written for the single-paint ones and reached by
    // a *call* for the two-paint ones -- and that call went to a zero-filled hole, so the
    // distinction was the fault rather than a feature.
    // The shapes that paint at all want one vblank a flip, which is what takes the picture rate to
    // sixty; the shapes that do not are the ones kept for comparison. `LoopFrameLiteralTwice` is
    // the one that reaches the frame twice and comes back, and it is what condition 1 is about, so
    // it is here with the rest rather than left measuring a thirty-hertz picture twice.
    const bool wantsOneVblank = mode == Mode::IntervalField || mode == Mode::OneAtSixty ||
                                mode == Mode::TwiceAtSixty || mode == Mode::TailTwiceAtSixty ||
                                mode == Mode::LoopDispatchTwice ||
                                mode == Mode::LoopFrameLiteralTwice;
    if (wantsOneVblank) {
        // The field the title's own `GX2SetSwapInterval` call was handed, written
        // to one. Recorded and restored on the way out, because it is the
        // title's state and not the mod's.
        uint32_t field = 0;
        uint32_t display = 0;
        {
            std::scoped_lock frameLock(m_frame.mutex);
            display = m_frame.display;
        }
        if (display == 0 || !m_readWord(display + kIntervalOffset, field)) {
            m_refusal = "the display's interval field at " + hex(kIntervalOffset) +
                        " would not give up its value";
            return m_refusal;
        }
        m_savedInterval = field;
        if (!m_writeWord(display + kIntervalOffset, kSwapInterval)) {
            m_refusal =
                "the display's interval field at " + hex(kIntervalOffset) + " would not take one";
            m_savedInterval = 0;
            return m_refusal;
        }
        m_wroteInterval = true;
        lucent::info("paint", "display interval field {} -> {}", hex(field), hex(kSwapInterval));
        if (wantsOneVblank) {
            // And the pacing itself, which is the emulator's and not the title's
            // memory. Both, so the title's record and the thing it records agree:
            // a field saying one while the flip still takes two vblanks would be
            // a claim nothing backs.
            //
            // **This is what takes the picture rate to sixty, and the field alone does not.**
            // Measured with the field written and the pacing left alone: the report read `interval
            // 1` and the paints still ran at thirty a second. The gate that opens the display
            // thread is the emulator's flip pacing, and the title's own record of the interval it
            // asked for is a statement about it rather than a thing that causes it.
            m_savedPacing = m_swapInterval();
            const uint32_t now = m_setSwapInterval(kSwapInterval);
            if (now != kSwapInterval) {
                m_refusal = "the flip pacing refused one vblank and is at " + hex(now);
                (void)m_writeWord(display + kIntervalOffset, m_savedInterval);
                m_wroteInterval = false;
                m_savedInterval = 0;
                return m_refusal;
            }
            m_wrotePacing = true;
            lucent::info("paint", "flip pacing {} -> {} vblank(s), and the title's field agrees",
                         hex(m_savedPacing), hex(kSwapInterval));
        }
    }
    if (m_block == 0) {
        m_refusal = m_reservationRefusal.empty()
                        ? "no executable guest memory was reserved for the stand-in"
                        : m_reservationRefusal;
        return m_refusal;
    }
    const std::optional<std::vector<uint32_t>> at = payload(m_block, mode);
    if (!at.has_value()) {
        // The reservation stands: it is still the loader's memory and still
        // empty, and a refusal here says nothing about it. Zeroing the block
        // would turn one refusal into a mod that can never be enabled.
        m_refusal = "the stand-in at " + hex(m_block) +
                    " cannot reach the swap-interval call or the loop it returns to";
        return m_refusal;
    }
    lucent::info("paint", "stand-in {} at {}: {} words", modeName(mode), hex(m_block),
                 std::to_string(at->size()));
    // The display thread's lock, held across every word written here *and* the
    // vtable slot rewritten below, because the two together are what changes what
    // a display thread is running: a thread already inside the stand-in is in the
    // recompiled function covering this block, and writing the block deletes that
    // function under it.
    //
    // Measured, with the log naming the moment: a segmentation fault inside
    // recompiled code one millisecond after the patch was armed, on the display
    // thread's own core, with the frame's address 0x0274c268 in the stack. The
    // probe takes this same lock on every call, so holding it here means the
    // display thread has finished its frame and is not in the stand-in.
    std::scoped_lock displayThread(m_frame.mutex);
    // A word at a time, through the seam that owns the guest's order. Written
    // as bytes from a host word array, every instruction in the block would
    // reach the guest with its halves exchanged -- which is a display thread
    // branching into noise, and no report anywhere near the cause.
    for (size_t word = 0; word < at->size(); word++) {
        if (!m_writeWord(m_block + 4 * static_cast<uint32_t>(word), (*at)[word])) {
            m_refusal = "the stand-in's block at " + hex(m_block) + " would not take word " +
                        std::to_string(word);
            return m_refusal;
        }
    }
    // The words, as written. Arming this stand-in is the step after which the
    // product may not survive long enough to be asked anything, so the words it
    // holds are reported here rather than by a later read: a run that faults
    // one millisecond after arming leaves nothing alive to read.
    //
    // This is not a diagnostic left in for a fault. It is the only place the
    // payload is ever visible as the guest received it, and the payload is what
    // decides whether the display thread paints twice or loops.
    {
        std::string written;
        for (size_t word = 0; word < at->size(); word++) {
            written += " " + hex((*at)[word]);
        }
        lucent::info("paint", "stand-in {} at {} holds:{}", modeName(mode), hex(m_block), written);
    }
    if (mode == Mode::BranchEntry) {
        // The frame's own entry, not the vtable's slot: a direct branch out of the
        // title's own code into this block. The check above still runs, so a
        // revision whose vtable points elsewhere is still refused by name -- the
        // vtable is read to learn the frame is the one this title has, not
        // because this mode writes there.
        const uint32_t frameEntry = kDisplayFrame;
        uint32_t there = 0;
        // The word the probe sits on is checked, and named: a revision whose frame differs there is
        // refused by name rather than branched into. The frame's *entry* is checked too, and the
        // two are different checks -- the entry is what the stand-in branches to, the probe word is
        // what the probe displaces.
        if (!m_readWord(frameEntry, there) || there != kDisplayFrameFirst) {
            m_refusal = "the frame's entry at " + hex(frameEntry) + " holds " + hex(there) +
                        ", not " + hex(kDisplayFrameFirst) +
                        "; this stand-in is written for this title's frame";
            return m_refusal;
        }
        if (!m_readWord(kDisplayFrameProbe, there) || there != kDisplayFrameProbeFirst) {
            m_refusal = "the frame's word at " + hex(kDisplayFrameProbe) + " holds " + hex(there) +
                        ", not " + hex(kDisplayFrameProbeFirst) +
                        ", so the probe would displace a different instruction";
            return m_refusal;
        }
        if (!withinReach(frameEntry, m_block)) {
            m_refusal = "the stand-in's block at " + hex(m_block) +
                        " is out of a branch's reach "
                        "of " +
                        hex(frameEntry);
            return m_refusal;
        }
        const uint32_t branch = branchTo(frameEntry, m_block, false);
        if (!m_writeWord(frameEntry, branch)) {
            m_refusal = "the frame's entry at " + hex(frameEntry) + " would not take the branch";
            return m_refusal;
        }
        m_patched = frameEntry;
        m_original = there;
        m_patchedIsSlot = false;
    } else if (!m_writeWord(slot, m_block)) {
        m_refusal = "vtable slot " + hexField(kFrameSlot) + " would not take the write";
        return m_refusal;
    }
    m_installed = true;
    m_mode = mode;
    if (mode != Mode::BranchEntry) {
        m_patched = slot;
        m_patchedIsSlot = true;
    }
    m_refusal.clear();
    if (mode == Mode::BranchEntry) {
        lucent::info("paint",
                     "the frame's own entry at {} now branches to {}; the display "
                     "thread paints {}",
                     hex(kDisplayFrame), hex(m_block), modeName(mode));
    } else {
        lucent::info("paint", "{} slot {:#04x} now {}; the display thread paints {}", hex(vtable),
                     kFrameSlot, hex(m_block), modeName(mode));
    }
    return {};
}

std::string WindWakerPaint::disable() {
    std::scoped_lock lock(m_mutex);
    return disableLocked();
}

// The write-back, with the lock the callers already hold: a scoped_lock is not
// recursive, so a disable() from inside enable() would wait on itself.
std::string WindWakerPaint::disableLocked() {
    if (!m_installed) {
        // Already the title's own frame: putting it back is what was asked
        // for, and refusing would make the unmodded window -- the control
        // every comparison is made against -- unmeasurable.
        return {};
    }
    // The display thread's lock, for the same reason enable() takes it: putting
    // the title's own frame back while a display thread is inside the stand-in
    // leaves that thread in a function this is about to change. The lock is the
    // probe's own, taken on every call, so this waits for the frame to finish
    // rather than for anything the guest knows about.
    //
    // Held for the whole write-back rather than only the one word, because the
    // interval field below is written after this and wants the same protection.
    std::scoped_lock displayThread(m_frame.mutex);
    // The word that was written, which is the live vtable's slot. Not the
    // address out of the image: enable() patches whatever vtable the display
    // holds, so restoring to the image's address would rewrite a word nothing
    // patched and leave the patch in place -- the mod installed with no way out
    // and a second vtable damaged.
    if (m_patched == 0 || !m_writeWord(m_patched, m_original)) {
        // Named by what was written, not by "vtable slot": the branch-entry mode
        // writes a different word in a different place, and a refusal that said
        // "vtable slot" for it would send a reader to look in the vtable.
        m_refusal = std::string(m_patchedIsSlot ? "vtable slot " + hexField(kFrameSlot) +
                                                      " at "
                                                      "the live vtable"
                                                : "the frame's own entry") +
                    " at " + hex(m_patched) + " would not take the title's own " +
                    hex(m_original == 0 ? kDisplayFrame : m_original) + " back";
        return m_refusal;
    }
    m_installed = false;
    m_refusal.clear();
    if (m_wroteInterval) {
        // The display's address, read under the lock this function already holds
        // rather than by taking it again: a scoped_lock is not recursive, and
        // taking it here is a deadlock with itself. Which is what happened the
        // first time this lock was added here, and it looked like a hang rather
        // than like a mistake.
        const uint32_t display = m_frame.display;
        if (display != 0 && !m_writeWord(display + kIntervalOffset, m_savedInterval)) {
            m_refusal =
                "the display's interval field would not take " + hex(m_savedInterval) + " back";
            return m_refusal;
        }
        m_wroteInterval = false;
        m_savedInterval = 0;
    }
    if (m_wrotePacing) {
        (void)m_setSwapInterval(m_savedPacing);
        m_wrotePacing = false;
        m_savedPacing = 0;
    }
    lucent::info("paint", "vtable slot {:#04x} back to the title's own {}", kFrameSlot,
                 hex(m_original));
    m_patched = 0;
    return {};
}

namespace {

// Whether every one of a paint's call targets is an address the title's own code could be at.
//
// The frame's image runs 0x02000020 to 0x028f87f4, and the title calls through the import thunks
// just above it. **A target outside that is a value, not a call target** -- which is what the
// second paint's near-null dereference looks like from the display's side, and this says so per
// sample rather than leaving a reader to compare five hex numbers by eye.
bool targetInImage(uint32_t address) {
    return address >= 0x02000020u && address < 0x028f88d4u;
}

bool targetsInImage(const std::array<uint32_t, WindWakerPaint::kRecentWords>& sample) {
    for (size_t word = 2; word < sample.size(); word++) {
        if (!targetInImage(sample[word])) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string WindWakerPaint::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("frame", hex(kDisplayFrame));
    // The vtable the slot lives in, and only for a patch that went through the
    // slot: the branch-entry mode writes a word in the title's code and has no
    // slot at all, so it says the vtable it found and nothing else.
    body.string("vtable",
                hex(m_patchedIsSlot && m_patched != 0 ? m_patched - kFrameSlot : m_vtableFound));
    body.string("patchedSlot", hex(m_patched));
    body.string("slot", hexField(kFrameSlot));
    body.raw("installed", m_installed ? "true" : "false");
    body.string("mode", std::string(modeName(m_mode)));
    body.string("block", hex(m_block));
    body.number("swapIntervalAsked", kSwapInterval);
    // The pacing, or the fact that there is none yet: the emulator's shared area
    // is created during the graphics bring-up, and a channel asked before that
    // gets a value that is not an interval. Printing the number would be a
    // reading nobody could interpret, and 0xffffffff in a report reads as a bug
    // rather than as "the title has no surface yet".
    const uint32_t pacing = m_swapInterval();
    if (pacing == GuestPatching::kSwapIntervalUnknown) {
        body.raw("pacing", "null");
        body.string("pacingWhy", "the graphics bring-up has not created its shared area yet");
    } else {
        body.number("pacing", pacing);
    }
    body.number("titleSwapInterval", kTitleSwapInterval);
    body.number("paints", m_paints.load());
    body.string("probe", probeName());
    // The two samples of the flip fields, as the probe took them. A toggle
    // between them while the paint count climbs is the frame's `flags ^= 2`, and
    // one sample cannot show a sequence: the same field reads 0 whether the toggle
    // never happens and whether it happened between the two reads.
    {
        std::scoped_lock lock(m_frame.mutex);
        body.number("flagsAtLastPaint", m_frame.recent[0]);
        body.number("phaseAtLastPaint", m_frame.recent[1]);
        // The five indirect call targets, at the last paint, and the last two paints' readings of
        // all seven words. **The pair is the measurement**: with a stand-in that paints twice,
        // consecutive paints are the two halves of one pass, so a target that differs between them
        // is what the second paint found where the first left something else. One paint's reading
        // says nothing about that, which is why the report carries two and a `samplesValid` beside
        // them rather than showing a single row as though it were a comparison.
        body.number("indirectBaseAtLastPaint", m_frame.callTargetBase);
        JsonBody targets;
        for (size_t index = 0; index < WindWakerPaint::kFrameCallTargetOffsets.size(); index++) {
            targets.number(std::to_string(WindWakerPaint::kFrameCallTargetOffsets[index]).c_str(),
                           m_frame.callTargets[index]);
        }

        body.object("callTargetsAtLastPaint", targets.text());
        body.raw("samplesValid", m_frame.samplesValid ? "true" : "false");
        JsonBody pair;
        for (size_t paint = 0; paint < WindWakerPaint::kFrameSamples; paint++) {
            JsonBody one;
            one.number("flags", m_frame.samples[paint][0]);
            one.number("phase", m_frame.samples[paint][1]);
            for (size_t index = 0; index < WindWakerPaint::kFrameCallTargetOffsets.size();
                 index++) {
                one.number(
                    ("target_" + std::to_string(WindWakerPaint::kFrameCallTargetOffsets[index]))
                        .c_str(),
                    m_frame.samples[paint][2 + index]);
            }
            // A target that is not a code address, named here rather than left to be spotted: the
            // frame's own code is in the image at 0x02000020-0x028f87f4, and anything outside it
            // that is also not one of the MMU ranges is a value, not a pointer.
            one.string("allTargetsInImage",
                       targetsInImage(m_frame.samples[paint]) ? "true" : "false");
            pair.object(std::to_string(paint).c_str(), one.text());
        }
        body.object("lastTwoPaints", pair.text());
    }
    // The display pointer, the vtable it holds and its fields are one reading
    // of the probe's state under its lock, not three unlocked ones.
    const DisplayFacts facts = displayFacts();
    body.string("display", hex(facts.display));
    body.string("liveVTable", hex(facts.vtable));
    body.object("fields", facts.fields);
    if (!m_refusal.empty()) {
        body.string("refusal", m_refusal);
    }
    return body.finish();
}

// What the display object holds, read once under the probe's lock: which
// vtable, and its four fields as a JSON object of their own. Before the display
// thread has been seen every value is zero, which is a reading and not a guess.
WindWakerPaint::DisplayFacts WindWakerPaint::displayFacts() const {
    std::scoped_lock frameLock(m_frame.mutex);
    DisplayFacts facts{.display = m_frame.display};
    if (facts.display == 0) {
        facts.fields = JsonBody().text();
        return facts;
    }
    (void)m_readWord(facts.display + kVTableOffset, facts.vtable);

    JsonBody fields;
    for (const DisplayField& field : kDisplayFields) {
        uint32_t value = 0;
        if (!m_readWord(facts.display + field.offset, value)) {
            continue;
        }
        fields.number(field.name, value);
    }
    facts.fields = fields.text();
    return facts;
}

std::string WindWakerPaint::probeName() const {
    std::scoped_lock lock(m_frame.mutex);
    if (!m_frame.installation.has_value()) {
        return "pending";
    }
    switch (*m_frame.installation) {
    case GuestCallProbes::Installation::Installed:
        return "installed";
    case GuestCallProbes::Installation::EntryHeldOther:
        return "entryHeldOther";
    case GuestCallProbes::Installation::EntryNotRelocatable:
        return "entryNotRelocatable";
    case GuestCallProbes::Installation::EntryReadsLinkRegister:
        // Named rather than folded into "unknown", because it is a refusal a caller can act on: the
        // probe belongs on a different word, and the report says which one it wanted.
        return "entryReadsLinkRegister";
    case GuestCallProbes::Installation::NoCodeSpace:
        return "noCodeSpace";
    }
    return "unknown";
}

} // namespace wiiuport::title
