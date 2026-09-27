#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"
#include "wiiuport/title/DrawAttributeCensus.h"
#include "wiiuport/title/NodePoseLocator.h"
#include "wiiuport/title/ObjectIdentityScope.h"
#include "wiiuport/title/ObjectPoseHistory.h"
#include "wiiuport/title/ObjectPoseLocator.h"
#include "wiiuport/title/UniformBlockAddress.h"
#include "wiiuport/title/UniformBlockRing.h"
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
    // the block's SIZE at +0x0c and its ADDRESS at +0x04.
    //
    // **The names here were backwards, and the measurement is what corrected them.** The
    // earlier sentence said "size at +0x04 and offset at +0x0c", and this file's own
    // `blockOf` said the opposite in a comment -- that the +0x04 word "read 0x3e634300 on a
    // real binding, a pointer, not a length", and that a tool taking it for a byte count "asked
    // the product for a gigabyte and the product died". Two contradictory stories about the
    // same two words, in one file, and I followed the header twice: a hand-off passed the
    // +0x0c word as an address and the +0x04 word as a size, and the run reported 186,133
    // bindings "past the bound of 4096" with nothing read at all.
    //
    // Measured, once, on the real title: **the word at +0x0c reads 0x40 for every object, and
    // 0x40 is 64 bytes**, which agrees with 233 whole-block scans of a 64-byte block finding no
    // rigid transform in one. So +0x0c is the size and +0x04 is the address -- and the binder's
    // decompilation agrees, modulo one thing worth stating: `FUN_027ff88c` passes `entry[0x0c]`
    // as the 2nd argument and `entry[0x04]` as the 3rd, while the fork's export maps its `gpr[4]`
    // to its own `size` and its `gpr[5]` to its `virtualAddress`, the reverse of the documented
    // GX2 order. So the *fork* is the odd one out, and it is the fork's reading -- address at
    // `+0x04` -- that these constants follow.
    //
    // **The names say which is which, and neither is a relative offset.** There is no base: the
    // fork's `_GX2SubmitUniformBlock` writes `memory_virtualToPhysical(entry[0x04])` into the
    // uniform block register with nothing added to it. `kEntryBlockSize` is the word a caller may
    // use as a length; `kEntryBlockAddress` is the word the address comes from, and reading it as
    // a *guest* address is the mistake this comment exists to prevent -- it is a physical one.
    static constexpr uint32_t kEntriesOffset = 0x10;
    static constexpr uint32_t kCursorOffset = 0x4c;
    static constexpr uint32_t kEntrySize = 0x1c;
    static constexpr uint32_t kEntryBlockAddress = 0x04;
    static constexpr uint32_t kEntryBlockSize = 0x0c;
    // How many words a descriptor entry is, all of them reported.
    static constexpr size_t kEntryWords = kEntrySize / sizeof(uint32_t);
    // The list is *allocated* with two entries -- a cursor above 0 is a reading this census reports
    // rather than one it explains -- but **only the first is mapped**: the entry at
    // `kEntriesOffset + kEntrySize` failed to read in 199,280 of 199,280 bindings. So this is the
    // allocated depth, not the readable one, and nothing may be read at the far slot as though it
    // were there.
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

    // Whether the previous tick's uniform block contents survive to the next tick's paint. The
    // objective's own second question, and it is measured here because the census is the one
    // place that knows which block a binding names. Null is allowed and reported as null.
    void setBlockRing(UniformBlockRing* ring);

    // Where the block is, and which word of the record says so. Null is allowed and reported
    // as null.
    void setBlockAddress(UniformBlockAddress* address);

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
        // Each slot's block address and size, taken from the words the binder's own
        // decompilation takes them from.
        //
        // `block` is **0 when no word of the record names the address**, which is what the
        // measurement currently says: five of the seven words read as guest memory, so
        // `addressWordByMapping` refuses rather than picking one, and a block of 0 is the
        // refusal. It was not always a refusal -- naming a word that read turned out to name the
        // record's own leading pointer, and the ring then compared the record with itself.
        uint32_t block = 0;
        uint32_t otherBlock = 0;
        uint32_t blockSize = 0;
        uint32_t otherBlockSize = 0;
        // Which words of the record read as guest addresses **on their own**, with nothing added.
        // Adding one word to every other word -- the first version -- assumed a base, and that
        // assumption is what made "word 0 reads" an artefact rather than a finding.
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
    uint32_t blockOf(const std::array<uint32_t, kEntryWords>& entry, uint32_t& size) const;

    // Which word of the record is the block's address, decided by whether the word reads as guest
    // memory on its own, or -1 when no word does in a large enough share of the bindings. -1 is
    // the honest answer for a record whose address is not one of its own words, which is what the
    // relative-offset reading implied and the decompilation refutes.
    int addressWordByMapping() const;

    // How large a share of the bindings a word must read in to be called the address. A majority
    // rather than a lead: several words of a record are small integers that land in mapped memory
    // often enough to lead by accident.
    static constexpr double kMappedWordShare = 0.5;
    void mapWords(const std::array<uint32_t, kEntryWords>& entry,
                  std::array<bool, kEntryWords>& mapped);

    Register m_register;
    ReadWord m_readWord;
    ObjectPoseHistory m_poseHistory;
    const ObjectPoseLocator* m_locator = nullptr;
    NodePoseLocator* m_nodes = nullptr;
    ObjectIdentityScope* m_scope = nullptr;
    const DrawAttributeCensus* m_drawAttributes = nullptr;
    const VertexPoseHistory* m_vertexHistory = nullptr;
    UniformBlockRing* m_ring = nullptr;
    UniformBlockAddress* m_address = nullptr;
    Binder m_first{*this, false};
    Binder m_second{*this, true};
    std::atomic<uint64_t> m_bindings{0};
    mutable std::mutex m_mutex;
    // A cursor histogram: how many bindings read each entry of the two.
    std::array<uint64_t, kEntries> m_cursors{};
    // Per word, how many bindings it read as guest memory, over `m_wordTests` bindings.
    std::array<std::atomic<uint64_t>, kEntryWords> m_wordReads{};
    std::atomic<uint64_t> m_wordTests{0};
    // Why a record was not handed to the address measurement. A silent skip is a pairing that
    // went wrong without saying so, and the first version of this published nothing at all for
    // four minutes of a run before anyone could see why.
    std::atomic<uint64_t> m_otherRecordsRead{0};
    std::atomic<uint64_t> m_otherRecordsUnread{0};
    std::atomic<uint64_t> m_recordsPublished{0};
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
