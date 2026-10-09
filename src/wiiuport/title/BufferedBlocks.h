#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

namespace wiiuport::title {

// Wind Waker HD's double-buffered g3d uniform blocks -- each model's view blocks, world matrices
// and the rest -- learned from the title's own commit of each block.
//
// An entry holds two GPU buffers. The commit (`0x027fb678`) makes the slot it filled the one draws
// bind, so the slot an entry's draws bind changes once per commit and the other still holds the
// entry's previous contents. The probe reads the entry as the commit begins; changes nothing.
class BufferedBlocks final : public GuestCallProbes::Probe {
  public:
    static constexpr uint32_t kCommit = 0x027fb678;
    static constexpr uint32_t kCommitFirstInstruction = 0x7c6c1b78; // or r12,r3,r3
    static constexpr size_t kEntryRegister = 3;
    // The entry: its vtable (the block's kind), two slots, and the slot the commit binds.
    static constexpr uint32_t kVtable = 0x0c;
    static constexpr uint32_t kSlots = 0x10;
    static constexpr uint32_t kSlotStride = 0x1c;
    static constexpr uint32_t kSlotBuffer = 0x04;
    static constexpr uint32_t kSlotSize = 0x0c;
    static constexpr uint32_t kFilled = 0x48;
    static constexpr uint32_t kEntryBytes = kFilled + 4;
    static constexpr uint32_t kSlotCount = 2;
    // The view blocks' kind; `0x027fb880` uploads them.
    static constexpr uint32_t kViewVtable = 0x1016ef54;

    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry, uint32_t resume);
    using GuestBytes = std::function<const void*(uint32_t address, uint32_t size)>;

    BufferedBlocks(Register registerProbe, GuestBytes guestBytes)
        : m_register(registerProbe), m_guestBytes(std::move(guestBytes)) {
    }

    // Registers with the fork, before the title is linked.
    void install();
    void OnInstall(GuestCallProbes::Installation installation) override;
    void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;

    struct Slot {
        uint32_t buffer = 0;
        uint32_t size = 0;
    };

    struct Entry {
        uint32_t vtable = 0;
        std::array<Slot, kSlotCount> slots{};
        uint64_t commits = 0;
        // The slot the last commit made the one draws bind.
        uint32_t bound = 0;
    };

    // Whose slot a buffer is, as of the last commit that named it.
    struct Binding {
        uint32_t entry = 0;
        uint32_t slot = 0;
    };

    std::optional<Entry> entry(uint32_t address) const;
    std::optional<Binding> bindingOf(uint32_t buffer) const;
    std::string json() const;

  private:
    Register m_register;
    GuestBytes m_guestBytes;
    mutable std::mutex m_mutex;
    std::optional<GuestCallProbes::Installation> m_installation;
    uint64_t m_commits = 0;
    uint64_t m_unreadable = 0;
    uint64_t m_slotOutOfRange = 0;
    std::map<uint32_t, Entry> m_entries;
    std::unordered_map<uint32_t, Binding> m_buffers;
};

} // namespace wiiuport::title
