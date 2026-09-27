#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wiiuport::title {

// Which field of a node holds its pose, found by shape and then by being constant across
// objects.
//
// Three measurements in a row say the pose is not in a uniform and not in the node: 233
// whole-block scans of the binder's 64-byte block across every 4-aligned offset found no
// transform, 756,350 assembled uniform buffers found two candidates and believed neither, and
// 8 distinct nodes at 4 scans of 1024 words each found no rigid transform at any offset in
// any of them. The project's own evidence says why -- the title positions geometry on the CPU
// each frame, which is why the shipped mechanism had to keep vertex bytes and blend them. So
// the transform is one the object itself carries, and this finds the field it lives in.
//
// **Where the probe goes, and why not where the vtable says.** The node's draw is
// `FUN_02160018`; the vtable at `0x10036300` slot `+0xc` holds `0x02160180`, which is 0x168
// bytes *into* the function. That address is not a safe probe site: its first instruction is
// `beq 0x02160190` (0x41820010), and a probe resumes at "the instruction after the entry", so
// a taken branch there would make the stub re-run the code the branch was there to skip --
// and the fork refuses such a site outright. The function's own entry is safe: its first word
// is `stwu r1,-0x148(r1)` (0x9421FEB8), which does not branch.
//
// **And the entry is never called**, which is measured rather than assumed: over two
// eight-second windows the probe took zero calls, because the title dispatches the draw only
// through the vtable's target. The second table that holds the entry, at `0x10010648`, is not
// the one in use. The probe is kept anyway, because `calls` being zero *is* that measurement;
// a report that said "no field was found" without saying the draw was never entered at that
// address would read as a locator that found nothing rather than one that was never asked.
//
// The node is in `r3` at the entry and in `r28` for the rest of the function -- the prologue
// copies it with `or r28,r3,r3` and the frame saves r24-r31 -- so at the entry `r3` is the
// node, and the register is named rather than implied.
//
// **The belief is cross-object, and that is what makes it a locator rather than a
// coincidence.** A pose field is at *one offset in every object*; a colour triple that
// happens to be near unit length is at a different offset in each. So an offset is named when
// a majority of the tracked objects held a transform there, and the count of objects per
// offset is in the report. An offset only one object holds is reported as itself and is not
// named.
class NodePoseLocator {
  public:
    // Which of the two things a binding names is being scanned.
    //
    // A binding hands over the **sub-object** -- the thing whose descriptor the binder walks,
    // at `+0x10` with its cursor at `+0x4c` -- and the node is that one fixed subtraction
    // away, at `node + 0xa1c`. The sub-object is the remaining place the draw's own code
    // names, and it is the one a binding reaches without any arithmetic at all.
    //
    // The two are scored **separately**, and that is the point. One table over both would let
    // a field at the same offset in a node and in its sub-object count twice towards the bar
    // and be named on the strength of one of them.
    enum class Kind : uint8_t {
        Node,
        SubObject,
        Count
    };

    // The node's draw, its entry, and the word the image holds there.
    static constexpr uint32_t kDraw = 0x02160018;
    static constexpr uint32_t kFirstInstruction = 0x9421feb8; // stwu r1,-0x148(r1)
    static constexpr uint32_t kNodeRegister = 3;
    // The vtable's target, which is 0x168 bytes in and is not probed. Reported so a run says
    // which address the title dispatches through.
    static constexpr uint32_t kVtableTarget = 0x02160180;
    // How far the node's sub-object sits from the node, as the draw's own code has it:
    // `addi r3,r28,0xa1c` immediately before the call into it.
    static constexpr uint32_t kSubObjectOffset = 0xa1c;

    static constexpr float kUnitTolerance = 0.01f;
    static constexpr float kPerpendicularTolerance = 0.01f;
    static constexpr size_t kPoseWords = 12;
    // **What counts as movement, and why it is not a bitwise test.** The first run of this
    // reported `moved 18` beside `biggest delta 0.000000`: the values differed in the last
    // mantissa bit and in no way a pose moves, so a bitwise test named a static basis matrix
    // at three offsets 1020 bytes apart. A pose is a value that changes *between two draws of
    // the same object*, which are a frame apart, so a change smaller than a thousandth of a
    // unit is not one. The bar is stated rather than tuned: it is the first number here that
    // a reader could reasonably want to argue with, so it is the one that says what it is.
    static constexpr float kMotionEpsilon = 1e-3f;
    // **How much of each is read, and why the two windows are disjoint.** The sub-object is
    // a field *of* the node, at `+0xa1c`, so a 4 KiB window from the node's base covers the
    // sub-object's first 1508 bytes as well -- and a pose in the sub-object then shows up in
    // the node's table 2588 bytes further on. The first run of this measured exactly that: a
    // field at the sub-object's `+80` reported at the node's `+2668`, one measurement
    // counted twice, which is the very thing the two tables exist to prevent.
    //
    // So the node is read over its own leading fields only, up to where the sub-object
    // begins, and the sub-object is read over its own 4 KiB from there. The windows cannot
    // overlap by construction, and each table says how wide it is.
    static constexpr uint32_t kScanWords = 1024;
    // The node's own fields run from its base to the sub-object it embeds: 0xa1c bytes.
    static constexpr uint32_t kNodeScanWords = kSubObjectOffset / 4;
    static_assert(kNodeScanWords * 4 <= kSubObjectOffset,
                  "the node's window must end where the sub-object begins, or the two tables "
                  "are one measurement counted twice");
    // How many distinct objects of each kind are tracked, and how many of each object's
    // draws are scanned. An object is scanned more than once so a value that does not move
    // can be told from one that does, and a handful of objects is enough for the cross-object
    // test.
    static constexpr size_t kObjects = 8;
    static constexpr size_t kScansPerObject = 4;

    // One offset, as seen at one object.
    //
    // `still` is the counterpart of `moved` and it is not a consolation: a candidate held by
    // every object that never moves is a basis, a normal, a colour basis -- the same shape,
    // the same offset, the same value for ever. Reporting only `moved` would have let those
    // pass for a pose, which is exactly what the first run did.
    struct Candidate {
        uint32_t offset = 0;
        uint32_t scans = 0;
        uint32_t compared = 0;
        uint32_t moved = 0;
        uint32_t still = 0;
        float biggestDelta = 0.0f;
        std::array<float, kPoseWords> last{};
        bool held = false;
    };

    // The fork's seams, injected so this is testable without a guest.
    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry, uint32_t resume);
    using ReadWords = bool (*)(uint32_t guestAddress, uint32_t* values, uint32_t count);

    NodePoseLocator(Register registerProbe, ReadWords readWords);

    // Registers the probe, before the title is linked. See the class comment for why a probe
    // that is never called is still installed.
    void install();

    // One object, at one of its draws, and which of the two things it is. The production
    // route is `Kind::SubObject` from the binder, with the node derived by subtraction; the
    // probed entry is the other one and never fires.
    void observe(uint32_t address, Kind kind = Kind::Node);

    std::string json() const;

    uint64_t calls() const {
        return m_calls;
    }

    // The word the entry held when the probe was asked for it, and the one the image has.
    //
    // The fork refuses the install when the entry does not hold the word the probe names and
    // calls that `entryHeldOther` -- which is true and says nothing about *why*. So both
    // words are read and reported: a refusal that cannot say what it found is a refusal the
    // reader has to go and reproduce.
    uint32_t entryWordAtInstall() const {
        return m_entryWord;
    }

    // The offset the report believes for one kind, or 0 when no offset was both held by a
    // majority of that kind's tracked objects *and* seen to move. An offset every object
    // holds that never changes is the same shape as a pose and is not one.
    uint32_t bestOffset(Kind kind = Kind::Node) const;

  private:
    // The same, with the lock already held. `json()` holds it and needs the number, and a
    // non-recursive mutex taken twice on one thread is a deadlock rather than an answer --
    // which is what the first version of this did, and it hung the test run.
    uint32_t bestOffsetLocked(Kind kind) const;
    // How many distinct objects of one kind are tracked: the denominator every bar for that
    // kind is taken against.
    uint32_t trackedOfKindLocked(Kind kind) const;
    // How many objects of one kind a majority bar needs: zero when fewer than two are
    // tracked, because one object cannot agree with another and the cross-object test is the
    // whole of the locator.
    uint32_t neededLocked(Kind kind) const;
    // One kind's cross-object table and its believed offset, as a JSON object.
    std::string reportFor(Kind kind) const;

    class Draw final : public GuestCallProbes::Probe {
      public:
        explicit Draw(NodePoseLocator& owner) : m_owner(owner) {
        }

        void OnInstall(GuestCallProbes::Installation installation) override;
        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;

      private:
        NodePoseLocator& m_owner;
    };

    // A rigid transform, tested.
    static bool isPose(const float* words);
    // Scans one object's memory and folds what it finds into that object's candidates.
    // The window is the kind's own, so a node and its sub-object are read over disjoint
    // ranges and one field cannot be reported as two.
    void scan(uint32_t address, Kind kind);
    // How wide one kind's window is, in words.
    static uint32_t windowWords(Kind kind);
    // Tracks, or refuses, one object; whether it is tracked and whether it has scans left is
    // a shared decision, so it is made under the lock. The read of the object's memory is
    // not, so a scan does not hold the display thread's lock while it reads.
    bool claimLocked(uint32_t address, Kind kind);
    static const char* nameOf(Kind kind);

    Register m_register;
    ReadWords m_readWords;
    Draw m_draw{*this};
    std::atomic<uint64_t> m_calls{0};
    mutable std::mutex m_mutex;

    // Per tracked object: what it is, and its candidates.
    struct Tracked {
        uint32_t address = 0;
        Kind kind = Kind::Node;
        uint32_t scans = 0;
        std::vector<Candidate> candidates;
    };

    std::vector<Tracked> m_tracked;
    // Per kind, the denominator the cross-object bar is taken against, and the objects
    // refused once it was reached. A bar without a denominator is a threshold, not a test, so
    // both are reported.
    uint32_t m_refusedOfKind[static_cast<size_t>(Kind::Count)]{};
    uint64_t m_unreadable = 0;
    uint32_t m_entryWord = 0;
    std::optional<GuestCallProbes::Installation> m_installation;
};

} // namespace wiiuport::title
