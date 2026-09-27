#include "wiiuport/title/UniformBlockCensus.h"

#include "wiiuport/title/JsonBody.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace wiiuport::title {

namespace {

std::string hex(uint32_t value) {
    std::array<char, 11> text{};
    std::snprintf(text.data(), text.size(), "0x%08x", value);
    return {text.data()};
}

std::string_view installationName(std::optional<GuestCallProbes::Installation> value) {
    if (!value.has_value()) {
        return "pending";
    }
    switch (*value) {
    case GuestCallProbes::Installation::Installed:
        return "installed";
    case GuestCallProbes::Installation::EntryHeldOther:
        return "entryHeldOther";
    case GuestCallProbes::Installation::EntryNotRelocatable:
        return "entryNotRelocatable";
    case GuestCallProbes::Installation::NoCodeSpace:
        return "noCodeSpace";
    }
    return "unknown";
}

} // namespace

UniformBlockCensus::UniformBlockCensus(Register registerProbe, ReadWord readWord,
                                       ObjectPoseHistory::ReadWords readWords,
                                       const ObjectPoseLocator* locator, NodePoseLocator* nodes)
    : m_register(registerProbe), m_readWord(readWord), m_poseHistory(readWords), m_locator(locator),
      m_nodes(nodes) {
}

void UniformBlockCensus::install() {
    m_register(kBinder, kFirstInstruction, m_first, true, 0);
    m_register(kBinderSecond, kFirstInstruction, m_second, true, 0);
}

void UniformBlockCensus::Binder::OnInstall(GuestCallProbes::Installation result) {
    std::scoped_lock lock(mutex);
    installation = result;
}

void UniformBlockCensus::Binder::OnCall(std::span<const uint32_t, 32> gpr,
                                        uint32_t /*returnAddress*/) {
    m_owner.record(gpr[3], m_second);
}

void UniformBlockCensus::record(uint32_t object, bool second) {
    // Both of the two things this binding names, each scored in its own table.
    //
    // The binder's argument is the node's **sub-object** -- the thing whose descriptor it
    // walks -- and the node is that one fixed subtraction away, from the draw's own
    // `addi r3,r28,0xa1c`. This is the production route, and it is necessary rather than
    // convenient: the draw's entry is called zero times a run, because the title dispatches
    // the draw through the vtable's target instead.
    if (m_nodes != nullptr) {
        m_nodes->observe(object, NodePoseLocator::Kind::SubObject);
        m_nodes->observe(object - NodePoseLocator::kSubObjectOffset, NodePoseLocator::Kind::Node);
    }
    // Counted before the lock: a binding on the display thread must not be able
    // to block behind a report being written.
    m_bindings.fetch_add(1, std::memory_order_relaxed);
    if (object == 0) {
        return;
    }
    Binding binding;
    binding.object = object;
    binding.read = m_readWord(object + kCursorOffset, binding.cursor);
    if (binding.read) {
        // The whole entry, word for word. A partial read is a partial answer and
        // is reported as unread rather than as zeroes, because a block that was
        // dumped from zeroes would look like a real one.
        readEntry(object, binding.cursor, binding.entry, binding.read);
    }
    if (binding.read) {
        mapWords(object, binding.entry, binding.mapped);
        // The other of the two, read the same way. This is the slot a blend
        // reads: if it still holds the previous tick's pose, the two ticks'
        // values are both in memory when the tick binds and the in-between frame
        // is a lerp of two reads rather than a replay of a recording.
        binding.otherCursor = 0;
        for (uint32_t slot = 1; slot < kEntries; slot++) {
            if (slot != binding.cursor) {
                binding.otherCursor = slot;
            }
        }
        readEntry(object, binding.otherCursor, binding.otherEntry, binding.otherRead);
        mapWords(object, binding.otherEntry, binding.otherMapped);
        binding.block = blockOf(binding.entry, binding.mapped, binding.blockSize);
        binding.otherBlock =
            blockOf(binding.otherEntry, binding.otherMapped, binding.otherBlockSize);
        // What those two blocks hold, read now, while the binder is about to hand
        // them to the GPU. The entry's word at `+0x04` is the block's own address --
        // the two slots of every object measured are exactly `0x100` apart, and the
        // word at `+0x0c` the binder passes to `GX2Set*UniformBlock` is `0x40` for
        // every one of them, a constant and so an offset within the block rather than
        // a size. Read before the lock, because a binding on the display thread must
        // not queue behind a report being written.
        m_poseHistory.observe(object, binding.entry[kEntrySizeOffset / 4],
                              binding.otherEntry[kEntrySizeOffset / 4]);
    }
    std::scoped_lock lock(m_mutex);
    if (std::find(m_seen.begin(), m_seen.end(), object) == m_seen.end()) {
        // Capped: a list of every object in a scene is a list of everything the
        // title has ever drawn, and the count is what the report needs.
        if (m_seen.size() < 4096) {
            m_seen.push_back(object);
            m_objects = m_seen.size();
        } else {
            m_objects = m_seen.size() + 1;
        }
    }
    if (binding.read && binding.cursor < static_cast<uint32_t>(kEntries)) {
        m_cursors[binding.cursor]++;
    } else {
        m_cursorsOutOfRange++;
    }
    // Whether the ring turns per bind or per frame. Only a binding of an object
    // already seen can be a switch, so the compared count is the denominator a
    // switch rate is read against -- a switch count with no denominator says
    // nothing at all, and a title that draws each object once would otherwise
    // report zero switches for a ring that works.
    const auto known =
        std::find_if(m_lastCursor.begin(), m_lastCursor.end(), [object](const auto& pair) {
            return pair.first == object;
        });
    if (known != m_lastCursor.end()) {
        m_cursorCompared++;
        if (known->second != binding.cursor) {
            m_cursorSwitches++;
        }
        known->second = binding.cursor;
    } else if (m_lastCursor.size() < 4096) {
        m_lastCursor.emplace_back(object, binding.cursor);
    }
    if (m_exampleCount < kExamples) {
        m_examples[m_exampleCount] = binding;
        m_exampleCount++;
    }
    (void)second;
}

void UniformBlockCensus::readEntry(uint32_t object, uint32_t cursor,
                                   std::array<uint32_t, kEntryWords>& entry, bool& read) const {
    const uint32_t at = object + kEntriesOffset + cursor * kEntrySize;
    bool complete = read;
    for (size_t word = 0; word < kEntryWords; word++) {
        uint32_t value = 0;
        complete = m_readWord(at + 4 * static_cast<uint32_t>(word), value) && complete;
        entry[word] = value;
    }
    read = complete;
}

// The block an entry names.
//
// The binder's own decompilation is followed rather than guessed at. It reads
// one entry *past* the last counted one -- `param_1 + 0x10 + *(int *)(param_1 +
// 0x4c) * 0x1c` -- and takes that entry's word at `+0x0c` as the block's offset
// and its word at `+0x04` as the size, handing both to GX2Set*UniformBlock.
//
// A guess was tried first: try every word of the entry, add the entry's own
// offset, and keep the first that reads. It finds an address -- and it finds the
// *object*, whose vtable and heap pointers read perfectly well, 0x140 before
// where the block is. That is what "an address that reads" is worth: nothing. So
// the offset is taken from the word the binder takes it from, and the only
// question left is what the offset is relative to, which the decompilation does
// not say and the report therefore says.
//
// The size is the entry's word at `+0x04`, which read 0x3e634300 on a real
// binding -- a pointer, not a length. A tool that took it for a byte count asked
// the product for a gigabyte and the product died, so it is reported as it
// stands and never used as a length.
uint32_t UniformBlockCensus::blockOf(const std::array<uint32_t, kEntryWords>& entry,
                                     std::array<bool, kEntryWords>& mapped, uint32_t& size) const {
    const uint32_t offset = entry[kEntryOffsetOffset / 4];
    size = entry[kEntrySizeOffset / 4];
    (void)mapped;
    // The offset is relative to a base the title set elsewhere, and the only
    // bases in hand are the other slot of the same ring and the object itself.
    // Both are reported rather than one being chosen, because choosing is what
    // made the first attempt name the object.
    return offset;
}

void UniformBlockCensus::mapWords(uint32_t object, const std::array<uint32_t, kEntryWords>& entry,
                                  std::array<bool, kEntryWords>& mapped) const {
    (void)object;
    // Which words, added to the offset, name memory the guest can read. The
    // offset is the entry's own word at +0x0c, which the binder passes to the GPU
    // and which is relative to a base the title set elsewhere -- so the entry is
    // the only place left to look for the address, and every word that reads is
    // reported rather than the first one that happened to work.
    uint32_t probe = 0;
    for (size_t word = 0; word < kEntryWords; word++) {
        const uint32_t base = entry[word] + entry[kEntryOffsetOffset / 4];
        mapped[word] = m_readWord(base, probe) && m_readWord(base + 4, probe);
    }
}

std::string UniformBlockCensus::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("binder", hex(kBinder));
    body.string("binderSecond", hex(kBinderSecond));
    body.string("probe", std::string(installationName(m_first.installation)));
    body.string("probeSecond", std::string(installationName(m_second.installation)));
    body.number("entries", kEntries);
    body.number("bindings", m_bindings.load());
    body.number("objects", m_objects);
    JsonBody cursors;
    for (int entry = 0; entry < kEntries; entry++) {
        cursors.number(std::to_string(entry).c_str(), m_cursors[entry]);
    }
    body.raw("cursors", cursors.text());
    body.number("cursorsOutOfRange", m_cursorsOutOfRange);
    body.number("cursorSwitches", m_cursorSwitches);
    body.number("cursorCompared", m_cursorCompared);
    JsonBody examples;
    for (size_t index = 0; index < m_exampleCount; index++) {
        const Binding& binding = m_examples[index];
        JsonBody one;
        one.number("cursor", binding.cursor);
        one.string("object", hex(binding.object));
        one.number("offset", binding.entry[kEntryOffsetOffset / 4]);
        one.number("size", binding.entry[kEntrySizeOffset / 4]);
        JsonBody words;
        for (size_t word = 0; word < kEntryWords; word++) {
            words.number(std::to_string(word).c_str(), binding.entry[word]);
        }
        one.raw("entry", words.text());
        JsonBody readable;
        for (size_t word = 0; word < kEntryWords; word++) {
            readable.raw(std::to_string(word).c_str(), binding.mapped[word] ? "true" : "false");
        }
        one.raw("readableAtOffset", readable.text());
        // The other slot, whole, because whether the previous tick's values are
        // still in memory when this tick binds is the question a blend rests on
        // and it is answered by reading that slot and not by assuming a ring.
        one.number("otherCursor", binding.otherCursor);
        one.number("otherOffset", binding.otherEntry[kEntryOffsetOffset / 4]);
        one.number("otherSize", binding.otherEntry[kEntrySizeOffset / 4]);
        JsonBody otherWords;
        for (size_t word = 0; word < kEntryWords; word++) {
            otherWords.number(std::to_string(word).c_str(), binding.otherEntry[word]);
        }
        one.raw("otherEntry", otherWords.text());
        JsonBody otherReadable;
        for (size_t word = 0; word < kEntryWords; word++) {
            otherReadable.raw(std::to_string(word).c_str(),
                              binding.otherMapped[word] ? "true" : "false");
        }
        one.raw("otherReadableAtOffset", otherReadable.text());
        // Both slots' block offsets, as the binder's own decompilation takes
        // them: the entry one past the counted ones, its word at +0x0c.
        //
        // These are offsets, not addresses, and the report says so: the title
        // binds them against a base it sets elsewhere, and naming an address
        // here is what made a first attempt report the object as the block.
        one.number("blockOffset", binding.block);
        one.number("otherBlockOffset", binding.otherBlock);
        one.raw("read", binding.read ? "true" : "false");
        examples.raw(std::to_string(index).c_str(), one.text());
    }
    body.raw("examples", examples.text());
    // The pose history, whole, as its own report: what the block the binder names
    // holds, binding after binding. Whether the title writes the pose before the bind
    // or after it is what decides whether a blend can be driven from the binder alone,
    // and it is counted rather than read off the code.
    body.object("poseHistory", m_poseHistory.json());
    // And the other half of the question, from the other place it can be answered: the
    // uniform buffer the game assembled for a named node's draw, which is where the
    // locator looks. Null when there is no locator, rather than a member that is absent
    // and reads as a route that has nothing to say.
    if (m_locator == nullptr) {
        body.raw("poseLocator", "null");
    } else {
        body.object("poseLocator", m_locator->json());
    }
    // And the third place, which is where two measurements in a row say the pose is: the
    // node's own memory, at the node's own draw. Null when there is no locator, rather than
    // a member that is absent and reads as a route with nothing to say.
    if (m_nodes == nullptr) {
        body.raw("nodePose", "null");
    } else {
        body.object("nodePose", m_nodes->json());
    }
    if (!m_refusal.empty()) {
        body.string("refusal", m_refusal);
    }
    return body.finish();
}

} // namespace wiiuport::title
