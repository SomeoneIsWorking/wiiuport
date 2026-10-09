#include "wiiuport/title/BufferedBlocks.h"

#include "wiiuport/guest/GuestWords.h"
#include "wiiuport/guest/ProbeInstallation.h"
#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <limits>

namespace wiiuport::title {

void BufferedBlocks::install() {
    m_register(kCommit, kCommitFirstInstruction, *this, true, 0);
}

void BufferedBlocks::OnInstall(GuestCallProbes::Installation installation) {
    std::scoped_lock lock(m_mutex);
    m_installation = installation;
}

void BufferedBlocks::OnCall(std::span<const uint32_t, 32> gpr, uint32_t /*returnAddress*/) {
    const uint32_t address = gpr[kEntryRegister];
    const void* bytes = m_guestBytes(address, kEntryBytes);
    std::scoped_lock lock(m_mutex);
    m_commits++;
    if (bytes == nullptr) {
        m_unreadable++;
        return;
    }
    const uint32_t filled = guest::guestWord(bytes, kFilled);
    if (filled >= kSlotCount) {
        m_slotOutOfRange++;
        return;
    }
    Entry& entry = m_entries[address];
    entry.vtable = guest::guestWord(bytes, kVtable);
    for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
        size_t at = kSlots + (static_cast<size_t>(slot) * kSlotStride);
        entry.slots.at(slot) = {.buffer = guest::guestWord(bytes, at + kSlotBuffer),
                                .size = guest::guestWord(bytes, at + kSlotSize)};
        m_buffers[entry.slots.at(slot).buffer] = {.entry = address, .slot = slot};
    }
    entry.commits++;
    entry.bound = filled;
}

std::optional<BufferedBlocks::Entry> BufferedBlocks::entry(uint32_t address) const {
    std::scoped_lock lock(m_mutex);
    auto found = m_entries.find(address);
    if (found == m_entries.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<BufferedBlocks::Binding> BufferedBlocks::bindingOf(uint32_t buffer) const {
    std::scoped_lock lock(m_mutex);
    auto found = m_buffers.find(buffer);
    if (found == m_buffers.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::string BufferedBlocks::json() const {
    struct Kind {
        uint64_t entries = 0;
        uint64_t commits = 0;
        uint32_t smallest = std::numeric_limits<uint32_t>::max();
        uint32_t largest = 0;
        uint32_t sample = 0;
    };

    std::scoped_lock lock(m_mutex);
    std::map<uint32_t, Kind> kinds;
    for (const auto& [address, entry] : m_entries) {
        Kind& kind = kinds[entry.vtable];
        kind.entries++;
        kind.commits += entry.commits;
        for (const Slot& slot : entry.slots) {
            kind.smallest = std::min(kind.smallest, slot.size);
            kind.largest = std::max(kind.largest, slot.size);
        }
        if (kind.sample == 0) {
            kind.sample = address;
        }
    }
    std::string list;
    for (const auto& [vtable, kind] : kinds) {
        JsonBody item;
        item.string("vtable", JsonBody::hex(vtable));
        item.number("entries", kind.entries);
        item.number("commits", kind.commits);
        item.number("smallestSlot", kind.smallest);
        item.number("largestSlot", kind.largest);
        item.string("sampleEntry", JsonBody::hex(kind.sample));
        list += (list.empty() ? "" : ",") + item.text();
    }
    JsonBody body;
    body.string("installation", std::string(guest::installationName(m_installation)));
    body.number("commits", m_commits);
    body.number("unreadable", m_unreadable);
    body.number("slotOutOfRange", m_slotOutOfRange);
    body.number("entries", m_entries.size());
    body.number("buffers", m_buffers.size());
    body.raw("kinds", "[" + list + "]");
    return body.finish();
}

} // namespace wiiuport::title
