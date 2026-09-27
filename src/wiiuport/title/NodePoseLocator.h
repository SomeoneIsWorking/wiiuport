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

// Which field of a node holds its pose, found by shape and then constant.
//
// Two measurements in a row say the pose is not in a uniform: 233 whole-block scans of the
// binder's 64-byte block across every 4-aligned offset found no transform, and 756,350
// assembled uniform buffers found two candidates and believed neither. The project's own
// evidence says why -- the title positions geometry on the CPU each frame, which is why the
// shipped mechanism had to keep vertex bytes and blend them. So the transform is the node's
// own, applied before the display list is built, and this finds the field it lives in.
//
// **Where the probe goes, and why not where the vtable says.** The node's draw is
// `FUN_02160018`; the vtable at `0x10036300` slot `+0xc` holds `0x02160180`, which is 0x168
// bytes *into* the function. That address is not a safe probe site: its first instruction is
// `beq 0x02160190` (0x41820010), and a probe resumes at "the instruction after the entry",
// so a taken branch would make the stub re-run the code the branch was there to skip. The
// function's own entry is safe -- its first word is `stwu r1,-0x148(r1)` (0x9421FEB8), which
// does not branch -- and a second table in the image, at `0x10010648`, dispatches through it
// as well, so a probe there sees the node's draw either way. **Which of the two the title
// actually calls is measured**, not assumed: `calls` is in the report and a run where it
// stays at zero is a run that reached no node's draw at all.
//
// The node is in `r3` at the entry and in `r28` for the rest of the function -- the prologue
// copies it with `or r28,r3,r3` and the frame saves r24-r31 -- so at the entry `r3` is the
// node, and the register is named rather than implied.
//
// **The belief is cross-node, and that is what makes it a locator rather than a coincidence.**
// A pose field is at *one offset in every node*; a colour triple that happens to be near
// unit length is at a different offset in each. So an offset is named when it held a
// transform in a majority of the tracked nodes, and the count of nodes per offset is in the
// report. A per-node offset that never agrees with another node's is reported as itself and
// is not named.
class NodePoseLocator {
  public:
    // The node's draw, its entry, and the word the image holds there.
    static constexpr uint32_t kDraw = 0x02160018;
    static constexpr uint32_t kFirstInstruction = 0x9421feb8; // stwu r1,-0x148(r1)
    static constexpr uint32_t kNodeRegister = 3;
    // The vtable's target, which is 0x168 bytes in and is not probed. Reported so a run
    // says which address the title dispatches through.
    static constexpr uint32_t kVtableTarget = 0x02160180;

    static constexpr float kUnitTolerance = 0.01f;
    static constexpr float kPerpendicularTolerance = 0.01f;
    static constexpr size_t kPoseWords = 12;
    // How much of a node is read. The draw reaches node+0xa28, so a node is at least that
    // big; 4 KiB covers the field with room and is a stated bound rather than a guess at a
    // node's size.
    static constexpr uint32_t kScanWords = 1024;
    // How many distinct nodes are tracked, and how many of each node's draws are scanned.
    // A node is scanned more than once so a value that does not move can be told from one
    // that does, and a handful of nodes are enough for the cross-node test.
    static constexpr size_t kNodes = 8;
    static constexpr size_t kScansPerNode = 4;

    // One offset, as seen at one node.
    struct Candidate {
        uint32_t offset = 0;
        uint32_t scans = 0;
        uint32_t compared = 0;
        uint32_t moved = 0;
        float biggestDelta = 0.0f;
        std::array<float, kPoseWords> last{};
        bool held = false;
    };

    // The fork's seams, injected so this is testable without a guest.
    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry, uint32_t resume);
    using ReadWords = bool (*)(uint32_t guestAddress, uint32_t* values, uint32_t count);

    NodePoseLocator(Register registerProbe, ReadWords readWords);

    // Registers the probe, before the title is linked.
    //
    // It installs, and it is never called: measured, the title dispatches the node's draw
    // only through the vtable's target, which is inside the function and whose first
    // instruction is a conditional branch, so it is not a relocatable probe site either.
    // The probe is kept because `calls` being zero is that measurement, and a report that
    // said "the node's draw was never entered" without saying so would read as a locator
    // that found nothing rather than one that was never asked.
    void install();

    // The node, at one of its draws. This is the way the locator is fed in practice: the
    // node's draw calls its own sub-object at `node + 0xa1c`, so a probe on that sub-object's
    // binder -- which is installed, and fires hundreds of thousands of times a run -- already
    // holds the node, one fixed subtraction away.
    //
    // Measured, this is necessary and not a convenience: the node's draw is reached only
    // through the vtable's target, which the fork refuses as a probe site because its first
    // instruction is a branch, so the function's own entry is the only other candidate and it
    // is never called.
    void observe(uint32_t node);

    // How far the node's sub-object sits from the node, as the draw's own code has it:
    // `addi r3,r28,0xa1c` immediately before the call into it.
    static constexpr uint32_t kSubObjectOffset = 0xa1c;

    std::string json() const;

    uint64_t calls() const {
        return m_calls;
    }

    // The word the entry held when the probe was asked for it, and the one the image has.
    //
    // The fork refuses the install when the entry does not hold the word the probe names,
    // and calls that `entryHeldOther` -- which is true and says nothing about *why*. So
    // both words are read and reported: a refusal that cannot say what it found is a
    // refusal the reader has to go and reproduce.
    uint32_t entryWordAtInstall() const {
        return m_entryWord;
    }

    // The offset the report believes, or 0 when no offset was held by a majority of the
    // tracked nodes.
    uint32_t bestOffset() const;
    // The same, with the lock already held. `json()` holds it and needs the number, and a
    // non-recursive mutex taken twice on one thread is a deadlock rather than an answer --
    // which is what the first version of this did, and it hung the test run.
    uint32_t bestOffsetLocked() const;

  private:
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
    // Scans one node's memory and folds what it finds into the candidates.
    void scan(uint32_t node);

    Register m_register;
    ReadWords m_readWords;
    Draw m_draw{*this};
    std::atomic<uint64_t> m_calls{0};
    mutable std::mutex m_mutex;

    // Per tracked node, its candidates and how many of its draws have been scanned.
    struct Node {
        uint32_t address = 0;
        uint32_t scans = 0;
        std::vector<Candidate> candidates;
    };

    std::vector<Node> m_nodes;
    uint64_t m_refused = 0;
    uint64_t m_unreadable = 0;
    uint32_t m_entryWord = 0;
    std::optional<GuestCallProbes::Installation> m_installation;
};

} // namespace wiiuport::title
