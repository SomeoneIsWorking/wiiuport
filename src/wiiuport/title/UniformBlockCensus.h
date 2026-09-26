#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

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

    // The fork's seams, injected so this is testable without a guest.
    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe);
    using ReadWord = bool (*)(uint32_t guestAddress, uint32_t& value);

    UniformBlockCensus(Register registerProbe, ReadWord readWord);

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
        std::array<uint32_t, kEntryWords> entry{};
    };

    // Called on the display thread, once per binding, with the object.
    void record(uint32_t object, bool second);

    Register m_register;
    ReadWord m_readWord;
    Binder m_first{*this, false};
    Binder m_second{*this, true};
    std::atomic<uint64_t> m_bindings{0};
    mutable std::mutex m_mutex;
    // A cursor histogram: how many bindings read each entry of the two.
    std::array<uint64_t, kEntries> m_cursors{};
    // And the ones that named something else, which the report carries as itself.
    uint64_t m_cursorsOutOfRange = 0;
    // How many distinct objects have been seen, so a histogram of one object's
    // parity is not read as the title's.
    uint64_t m_objects = 0;
    std::vector<uint32_t> m_seen;
    std::array<Binding, kExamples> m_examples{};
    size_t m_exampleCount = 0;
    std::string m_refusal;
};

} // namespace wiiuport::title
