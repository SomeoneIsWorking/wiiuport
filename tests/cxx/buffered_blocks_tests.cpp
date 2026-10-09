// The double-buffered g3d blocks, learned from the title's commit of each.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/BufferedBlocks.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

using wiiuport::title::BufferedBlocks;

constexpr uint32_t kEntry = 0x3b7a0040;
constexpr uint32_t kBufferA = 0x4a001000;
constexpr uint32_t kBufferB = 0x4a001200;
constexpr uint32_t kBlockSize = 0x1c8;

void noRegistration(uint32_t /*entry*/, uint32_t /*firstInstruction*/,
                    GuestCallProbes::Probe& /*probe*/, bool /*holdsEntry*/, uint32_t /*resume*/) {
}

// A view entry as the commit finds it, big-endian, with `filled` the slot it is about to bind.
std::array<uint8_t, BufferedBlocks::kEntryBytes> entryBytes(uint32_t filled) {
    std::array<uint8_t, BufferedBlocks::kEntryBytes> bytes{};
    auto put = [&bytes](size_t at, uint32_t word) {
        bytes.at(at) = static_cast<uint8_t>(word >> 24);
        bytes.at(at + 1) = static_cast<uint8_t>(word >> 16);
        bytes.at(at + 2) = static_cast<uint8_t>(word >> 8);
        bytes.at(at + 3) = static_cast<uint8_t>(word);
    };
    put(BufferedBlocks::kVtable, BufferedBlocks::kViewVtable);
    put(BufferedBlocks::kSlots + BufferedBlocks::kSlotBuffer, kBufferA);
    put(BufferedBlocks::kSlots + BufferedBlocks::kSlotSize, kBlockSize);
    put(BufferedBlocks::kSlots + BufferedBlocks::kSlotStride + BufferedBlocks::kSlotBuffer,
        kBufferB);
    put(BufferedBlocks::kSlots + BufferedBlocks::kSlotStride + BufferedBlocks::kSlotSize,
        kBlockSize);
    put(BufferedBlocks::kFilled, filled);
    return bytes;
}

// The guest memory the probe reads: one entry, whose fill slot the test sets per commit.
class Guest {
  public:
    std::array<uint8_t, BufferedBlocks::kEntryBytes> entry = entryBytes(0);

    BufferedBlocks blocks() {
        return BufferedBlocks{&noRegistration,
                              [this](uint32_t address, uint32_t size) -> const void* {
                                  if (address != kEntry || size > entry.size()) {
                                      return nullptr;
                                  }
                                  return entry.data();
                              }};
    }

    void commit(BufferedBlocks& blocks, uint32_t filled) {
        entry = entryBytes(filled);
        call(blocks, kEntry);
    }

    static void call(BufferedBlocks& blocks, uint32_t address) {
        std::array<uint32_t, 32> gpr{};
        gpr[BufferedBlocks::kEntryRegister] = address;
        blocks.OnCall(gpr, 0);
    }
};

void aCommitRecordsTheEntrysTwoBuffersAndTheSlotItBinds() {
    Guest guest;
    BufferedBlocks blocks = guest.blocks();
    guest.commit(blocks, 1);
    const auto entry = blocks.entry(kEntry);
    check::isTrue(entry.has_value(), "a committed entry is known");
    if (!entry.has_value()) {
        return;
    }
    check::equal(entry->vtable, BufferedBlocks::kViewVtable, "with its kind");
    check::equal(entry->slots[0].buffer, kBufferA, "its first buffer");
    check::equal(entry->slots[1].buffer, kBufferB, "its second");
    check::equal(entry->slots[1].size, kBlockSize, "and their size");
    check::equal(entry->bound, uint32_t{1}, "and the slot draws now bind");
}

void eachBufferNamesItsEntryAndSlot() {
    Guest guest;
    BufferedBlocks blocks = guest.blocks();
    guest.commit(blocks, 0);
    const auto b = blocks.bindingOf(kBufferB);
    check::isTrue(b.has_value() && b->entry == kEntry && b->slot == 1,
                  "a bound buffer is found as its entry's slot");
    check::isTrue(!blocks.bindingOf(0x4a002000).has_value(), "and another buffer is no one's");
}

void theBoundSlotFollowsEachCommit() {
    Guest guest;
    BufferedBlocks blocks = guest.blocks();
    guest.commit(blocks, 0);
    guest.commit(blocks, 1);
    const auto entry = blocks.entry(kEntry);
    check::isTrue(entry.has_value() && entry->bound == 1 && entry->commits == 2,
                  "two commits, and the second's slot is the one bound");
}

void anUnreadableOrMalformedEntryIsCountedNotRecorded() {
    Guest guest;
    BufferedBlocks blocks = guest.blocks();
    Guest::call(blocks, 0x3b7a0100);
    guest.commit(blocks, 2);
    check::isTrue(!blocks.entry(kEntry).has_value() && !blocks.entry(0x3b7a0100).has_value(),
                  "neither an unreadable entry nor a third slot is recorded");
    const std::string report = blocks.json();
    check::isTrue(report.find("\"unreadable\":1") != std::string::npos &&
                      report.find("\"slotOutOfRange\":1") != std::string::npos,
                  "and each is counted");
}

} // namespace

void wiiuport::tests::runBufferedBlocksTests() {
    aCommitRecordsTheEntrysTwoBuffersAndTheSlotItBinds();
    eachBufferNamesItsEntryAndSlot();
    theBoundSlotFollowsEachCommit();
    anUnreadableOrMalformedEntryIsCountedNotRecorded();
}
