#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"
#include "wiiuport/title/DrawAttributeCensus.h"
#include "wiiuport/title/NodePoseLocator.h"
#include "wiiuport/title/ObjectIdentityScope.h"
#include "wiiuport/title/ObjectPoseHistory.h"
#include "wiiuport/title/ObjectPoseLocator.h"
#include "wiiuport/title/VertexPoseHistory.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wiiuport::title {

// What the title's own per-object uniform block binder does, counted.
//
// The binder is one of the two sub-objects a node's draw calls, and all it does
// is work out where the object's uniform block lives and bind it per stage:
//
//     entry = object + 0x10 + *(int *)(object + 0x4c) * 0x1c;
//     GX2Set{Vertex,Geometry,Pixel}UniformBlock(index, *(u32 *)(entry + 0xc),
//                                               *(u32 *)(entry + 4));
//
// The object's own constructor allocates that list with *two* entries and starts
// the cursor at one, which is the title double-buffering a per-object block --
// and whether the cursor alternates per frame is the question a blend turns on,
// because the answer decides if the previous tick's block is still there to read
// when this tick paints. Reading that out of the code means finding whoever moves
// the cursor, and the sub-object's methods are dispatched through a vtable that
// has no references to follow.
//
// So it is measured instead: a probe on the binder, and the cursor read out of
// the object the binder is handed, once per call. A cursor that alternates is
// the title keeping two; one that does not is the title reusing one. Neither is
// assumed, and a run where the probe was never called says so rather than
// reporting an empty histogram as a finding.
class UniformBlockCensus {
  public:
    static constexpr uint32_t kBlockWords = 96;
    static constexpr size_t kDifferingExamples = 12;
    // A differing offset is reported with the two values, so "differs" is a
    // measurement over a stated pair rather than a count.
    static constexpr size_t kDifferingShown = 6;

    // The two binders, which are the same function apart from which triple of
    // block indices they read, and the first instruction of each.
    static constexpr uint32_t kBinder = 0x027ff88c;
    static constexpr uint32_t kBinderSecond = 0x027ff9c0;
    static constexpr uint32_t kFirstInstruction = 0x7c0802a6;
    // The object's own layout, read out of its constructor: the descriptor array
    // at +0x10, the cursor at +0x4c, entries of 0x1c bytes, and within an entry
    // the block's size at +0x04 and offset at +0x0c.
    static constexpr uint32_t kEntriesOffset = 0x10;
    static constexpr uint32_t kCursorOffset = 0x4c;
    static constexpr uint32_t kEntrySize = 0x1c;
    static constexpr uint32_t kEntrySizeOffset = 0x04;
    static constexpr uint32_t kEntryOffsetOffset = 0x0c;
    // How many words a descriptor entry is, all of them reported.
    static constexpr size_t kEntryWords = kEntrySize / sizeof(uint32_t);
    // The list is allocated with two entries; a cursor above that is a reading
    // this census reports rather than one it explains.
    static constexpr int kEntries = 2;
    // How many worked examples the report carries, each one a binding with the
    // cursor, the block's offset and the block's size.
    static constexpr size_t kExamples = 8;

    // How many words of a block are read when a binding is examined, and how many
    // examples of the differing offsets the report carries.
    //
    // Reading the whole block and reporting the offsets that differ between the
    // two slots is the measurement that answers "which field holds the pose": it
    // does not need the field to be named in advance, and a run that finds
    // nothing differing says the two slots hold the same values rather than
    // leaving the question open.

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
    using ReadWord = bool (*)(uint32_t guestAddress, uint32_t& value);

    // `locator` is the other half of the same question, from the other place it could be
    // answered: the census reads the guest block the binder names, the locator reads the
    // uniform buffer the game assembled for a named node's draw. Null is allowed and
    // reported as null, because a census that requires a locator cannot be built without
    // one and there is no reason it should have to be.
    // Where a binding's object is published for the assembly hook to read, so a recorded
    // assembly carries the node it belongs to rather than the addresses it happened to read.
    // Null is allowed and means no publication, and a report then says so rather than implying
    // the assemblies have no object.
    void setIdentityScope(ObjectIdentityScope* scope);

    // The draw attribute census, reported beside the pose search. Null is allowed and reported
    // as null, so a build that wires no census does not look like a run that found nothing.
    void setDrawAttributeCensus(const DrawAttributeCensus* draws);

    // The two-tick vertex history, reported beside the attribute census it reads. Null is
    // allowed and reported as null.
    void setVertexPoseHistory(const VertexPoseHistory* history);

    UniformBlockCensus(Register registerProbe, ReadWord readWord,
                       ObjectPoseHistory::ReadWords readWords,
                       const ObjectPoseLocator* locator = nullptr,
                       NodePoseLocator* nodes = nullptr);

    // Registers both binder probes, before the title is linked.
    void install();

    // Bindings counted, and whether the probe is in place. A run that reached
    // gameplay with no binder call would otherwise report zeroes.
    uint64_t bindings() const {
        return m_bindings;
    }

    // What the binder did: the report.
    std::string json() const;

  private:
    class Binder final : public GuestCallProbes::Probe {
      public:
        Binder(UniformBlockCensus& owner, bool second) : m_owner(owner), m_second(second) {
        }

        void OnInstall(GuestCallProbes::Installation installation) override;
        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;
        mutable std::mutex mutex;
        std::optional<GuestCallProbes::Installation> installation;

      private:
        UniformBlockCensus& m_owner;
        bool m_second;
    };

    // One binding, as read out of the object the binder was handed: the
    // cursor, and the descriptor entry whole. The entry is seven words and the
    // binder uses two of them, so the other five are reported as they lie --
    // which is how the block's *address* gets found, since what the binder
    // passes the GPU is an offset into a region the title set elsewhere and a
    // relative offset alone cannot dump.
    struct Binding {
        bool read = false;
        uint32_t cursor = 0;
        // The object the binder was handed. Without it the other slot cannot be
        // read: the entries live at `object + 0x10 + cursor * 0x1c`, so the ring
        // is only a ring from the object that owns it.
        uint32_t object = 0;
        std::array<uint32_t, kEntryWords> entry{};
        // Each slot's block *offset* and size, taken from the words the binder's
        // own decompilation takes them from.
        //
        // Offsets and not addresses: the title binds them against a base it sets
        // elsewhere. Resolving one to an address by trying entry words until one
        // reads finds the object itself, whose vtable reads perfectly well -- a
        // wrong answer that looks like a right one, and which is what the first
        // attempt did.
        uint32_t block = 0;
        uint32_t otherBlock = 0;
        uint32_t blockSize = 0;
        uint32_t otherBlockSize = 0;
        // Which of those words, added to the entry's offset, is somewhere the
        // guest can actually read. The binder hands the GPU a *relative* offset,
        // so the block's address is a base the title set elsewhere, and the
        // entry is the only place left to look for it. This does not decide
        // which word it is: it reports which ones read, and what they hold is
        // then dumped and looked at.
        std::array<bool, kEntryWords> mapped{};
        // The other of the two entries, read the same way, because the question
        // the ring exists to answer is what the *other* slot holds: if it still
        // holds the previous tick's pose when this tick binds, a blend has two
        // sets of values to read and needs nothing replayed. Its cursor is the
        // one this binding did not use.
        bool otherRead = false;
        uint32_t otherCursor = 0;
        std::array<uint32_t, kEntryWords> otherEntry{};
        std::array<bool, kEntryWords> otherMapped{};
    };

    // Called on the display thread, once per binding, with the object.
    void record(uint32_t object, bool second);

    // What the block's pose holds, binding after binding.
    //
    // Handed the two blocks the descriptor names at the moment the binder is about to
    // use them, because the binder is the only place per object per frame where both
    // are known and where the title's own draw is about to consume them. It is a
    // separate owner rather than more of this one: the census answers what the binder
    // *does* with a descriptor, and this answers what the block it names *holds*.
    const ObjectPoseHistory& poseHistory() const {
        return m_poseHistory;
    }

    // One slot of the ring, read whole: the seven words of its entry, and then
    // which of those words, added to the entry's offset, names memory the guest
    // can read. Both are needed for *both* slots -- the bound one, to find the
    // block the tick is drawing from, and the other, to find the block the
    // previous tick drew from.
    void readEntry(uint32_t object, uint32_t cursor, std::array<uint32_t, kEntryWords>& entry,
                   bool& read) const;
    uint32_t blockOf(const std::array<uint32_t, kEntryWords>& entry,
                     std::array<bool, kEntryWords>& mapped, uint32_t& size) const;
    void mapWords(uint32_t object, const std::array<uint32_t, kEntryWords>& entry,
                  std::array<bool, kEntryWords>& mapped) const;

    Register m_register;
    ReadWord m_readWord;
    ObjectPoseHistory m_poseHistory;
    const ObjectPoseLocator* m_locator = nullptr;
    NodePoseLocator* m_nodes = nullptr;
    ObjectIdentityScope* m_scope = nullptr;
    const DrawAttributeCensus* m_drawAttributes = nullptr;
    const VertexPoseHistory* m_vertexHistory = nullptr;
    Binder m_first{*this, false};
    Binder m_second{*this, true};
    std::atomic<uint64_t> m_bindings{0};
    mutable std::mutex m_mutex;
    // A cursor histogram: how many bindings read each entry of the two.
    std::array<uint64_t, kEntries> m_cursors{};
    // And the ones that named something else, which the report carries as itself.
    uint64_t m_cursorsOutOfRange = 0;
    // Whether the ring turns per bind or per frame, counted rather than read:
    // a switch is a binding of an object already seen whose cursor differs from
    // the one it used last time, and `compared` is how many bindings could have
    // been one. A switch rate near one means the slot alternates every bind and
    // the two are two passes; near zero means it alternates per frame, or never.
    uint64_t m_cursorSwitches = 0;
    uint64_t m_cursorCompared = 0;
    // The last cursor each object used, so the next binding of the same object
    // can be compared with it. A flat map rather than a hash table: the objects
    // are few and the cap bounds the work.
    std::vector<std::pair<uint32_t, uint32_t>> m_lastCursor;
    // How many distinct objects have been seen, so a histogram of one object's
    // parity is not read as the title's.
    uint64_t m_objects = 0;
    std::vector<uint32_t> m_seen;
    std::array<Binding, kExamples> m_examples{};
    size_t m_exampleCount = 0;
    std::string m_refusal;
};

} // namespace wiiuport::title
