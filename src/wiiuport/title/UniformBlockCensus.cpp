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

void UniformBlockCensus::setDrawAttributeCensus(const DrawAttributeCensus* draws) {
    m_drawAttributes = draws;
}

void UniformBlockCensus::setVertexPoseHistory(const VertexPoseHistory* history) {
    m_vertexHistory = history;
}

void UniformBlockCensus::setBlockRing(UniformBlockRing* ring) {
    m_ring = ring;
}

void UniformBlockCensus::setBlockAddress(UniformBlockAddress* address) {
    m_address = address;
}

void UniformBlockCensus::setIdentityScope(ObjectIdentityScope* scope) {
    m_scope = scope;
}

void UniformBlockCensus::record(uint32_t object, bool second) {
    // Publish the object before anything else, so the draw's own uniform uploads -- which
    // follow this call, on this thread, inside this draw -- read the right one. A publication
    // that happens after the scan below would be a publication too late to matter.
    if (m_scope != nullptr) {
        m_scope->bind(object);
    }
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
    // The block's SIZE, handed to the ring. Not its address: the +0x04 word is a *relative*
    // offset (this file's own `blockOf` says so, and says that reading it as a length once
    // asked the product for a gigabyte), and the base the title set elsewhere has not been
    // identified. So the size goes over -- 64 bytes, measured, agreeing with 233 whole-block
    // scans of a 64-byte block -- and the address does not, because a relative offset read as a
    // guest address is a wrong answer rather than a missing one.
    if (binding.read) {
        mapWords(binding.entry, binding.mapped);
        // Counted for the bound slot only. The other slot's words are counted too and would
        // double every number here while describing a record that was not the one bound, so the
        // address word is decided from the record the binder was actually holding.
        for (size_t word = 0; word < kEntryWords; word++) {
            if (binding.mapped[word]) {
                m_wordReads[word]++;
            }
        }
        m_wordTests++;
        // The ring, which needs a *place* to re-read. Placed here rather than before the mapping so
        // that this binding has already counted towards the answer: the word that is the address is
        // decided by measurement (`addressWordByMapping`) rather than assumed, so until a word has
        // been shown to name guest memory in a majority of bindings the ring is told the size and
        // nothing else and reports zero comparisons -- which is honest where naming an offset is
        // not.
        if (m_ring != nullptr) {
            const int word = addressWordByMapping();
            if (word < 0) {
                m_ring->bindSize(object, binding.entry[kEntryBlockSize / 4]);
            } else {
                m_ring->bind(object, binding.entry[static_cast<size_t>(word)],
                             binding.entry[kEntryBlockSize / 4]);
            }
        }
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
        // Both records, published whole. The word that names the block's address is decided by
        // which of them the draw's real addresses match, and both slots are quoted raw so the
        // decision can be read off two records of one object rather than taken on trust.
        if (binding.otherRead) {
            m_otherRecordsRead.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_otherRecordsUnread.fetch_add(1, std::memory_order_relaxed);
        }
        // Published from the bound record alone. Waiting on the other slot published nothing at
        // all -- 179,597 of 179,597 other-slot reads failed, because that entry is not mapped --
        // and the measurement that was supposed to name the address word never ran at all.
        if (m_address != nullptr) {
            m_address->publish(object, std::span<const uint32_t>(binding.entry));
            m_recordsPublished.fetch_add(1, std::memory_order_relaxed);
            // The block's size, from the record's own size word, so the address measurement can
            // tell a register slot the guest wrote from one it did not: the register holds
            // `size - 1`. Read from the record rather than from `binding.blockSize`, which is
            // filled in by `blockOf` further down and is still zero here.
            m_address->setExpectedSize(binding.entry[UniformBlockCensus::kEntryBlockSize / 4]);
        }
        mapWords(binding.otherEntry, binding.otherMapped);
        binding.block = blockOf(binding.entry, binding.blockSize);
        binding.otherBlock = blockOf(binding.otherEntry, binding.otherBlockSize);
        // What the bound block holds, read now, while the binder is about to hand it to the GPU.
        //
        // **The `0x100`-apart claim is withdrawn.** It came from `mapWords`, which added one word
        // to every other word and so produced differences out of unmapped memory; the second
        // entry is not mapped at all (199,280 of 199,280 reads failed), so there is no second
        // block here and nothing to be 0x100 from. What is passed is the record's own size word
        // and the same word from the other record, which is the only pair of sizes this binder
        // hands over. Read before the lock, because a binding on the display thread must not queue
        // behind a report being written.
        m_poseHistory.observe(object, binding.entry[kEntryBlockSize / 4],
                              binding.otherEntry[kEntryBlockSize / 4]);
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
                                     uint32_t& size) const {
    // **There is no base.** The binder at 0x027ff88c / 0x027ff9c0 decompiles to
    // `GX2SetVertexUniformBlock(iVar5, uVar4, uVar6)` with `uVar4 = entry[0x0c/4]` and
    // `uVar6 = entry[0x04/4]`, and the fork's `_GX2SubmitUniformBlock` writes one of those two
    // straight into the uniform block register as `memory_virtualToPhysical(...)` with nothing
    // added to it. So the two words are the address and the size, in one order or the other, and
    // the earlier reading of `+0x04` as a relative offset -- with a base to be found -- was the
    // difference of a size and an address. The word that is the address is decided by measurement
    // (`addressWordByMapping`, and `title::UniformBlockAddress` by a second route) and not here.
    size = entry[kEntryBlockSize / 4];
    const int word = addressWordByMapping();
    return word < 0 ? 0 : entry[static_cast<size_t>(word)];
}

void UniformBlockCensus::mapWords(const std::array<uint32_t, kEntryWords>& entry,
                                  std::array<bool, kEntryWords>& mapped) {
    // Each word tested as an address on its own, with nothing added to it.
    //
    // The first version added the word at `+0x04` to every other word, which assumed what it was
    // trying to find: that one of them was a base. That assumption is what produced 233 whole-block
    // scans all agreeing, because `word + 0x40`-ish arithmetic lands in mapped memory often enough
    // to look like a hit. Read straight, a word either is the address or is not, and the count of
    // how often each one reads is what names it -- no base needed and none assumed.
    uint32_t probe = 0;
    for (size_t word = 0; word < kEntryWords; word++) {
        const uint32_t at = entry[word];
        mapped[word] = m_readWord(at, probe) && m_readWord(at + 4, probe);
    }
}

int UniformBlockCensus::addressWordByMapping() const {
    // The word that reads as guest memory in a large share of the bindings. Every word is tested
    // the same way on the same bindings, so this is a majority rather than a lead: a lead is a
    // guess with a number on it, and several words of a record are small integers that land in
    // mapped memory often enough to lead by accident.
    if (m_wordTests == 0) {
        return -1;
    }
    // **Exactly one word, or nothing.** Measured: five of the record's seven words read as guest
    // memory in every one of 179,597 bindings, and word 3 in none of them. A route that returns
    // the first word to clear a majority therefore returns word 0 -- the record's own leading
    // pointer -- and the ring then re-reads the descriptor instead of the block, which is exactly
    // what it did: 16 of 16 comparisons agreed, because it was comparing the record with itself.
    //
    // So a majority is not the bar here; *being the only one* is. With five words reading, this
    // route has no answer to give and says so, and the word is named instead by
    // `title::UniformBlockAddress`, which compares the record's words against the block addresses
    // the draw actually sourced -- a question where a wrong word loses rather than ties.
    int found = -1;
    for (size_t word = 0; word < kEntryWords; word++) {
        if (static_cast<double>(m_wordReads[word]) / static_cast<double>(m_wordTests.load()) >=
            kMappedWordShare) {
            if (found >= 0) {
                return -1;
            }
            found = static_cast<int>(word);
        }
    }
    return found;
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
    body.number("otherRecordsRead", m_otherRecordsRead.load());
    body.number("otherRecordsUnread", m_otherRecordsUnread.load());
    body.number("recordsPublished", m_recordsPublished.load());
    // The address word by the mapping route, with every word's share beside it: this is the
    // second, independent answer to which word is the address, and the two routes agreeing is
    // what makes it an answer rather than a coincidence that repeated.
    {
        const uint64_t tests = m_wordTests.load();
        const int named = addressWordByMapping();
        body.number("addressWordTests", tests);
        size_t candidates = 0;
        for (const auto& reads : m_wordReads) {
            if (tests != 0 && static_cast<double>(reads.load()) / static_cast<double>(tests) >=
                                  kMappedWordShare) {
                candidates++;
            }
        }
        body.number("addressWordCandidates", candidates);
        if (named < 0) {
            body.raw("addressWord", "null");
            // `string` and not `raw`: a bare word is not JSON, and a report that has to be parsed
            // to be read is a report that can fail to parse.
            body.string("addressWordRefused",
                        candidates == 0 ? "noWordReads" : "severalWordsReadSoNoneIsDistinguished");
            uint64_t best = 0;
            for (const auto& reads : m_wordReads) {
                best = std::max(best, reads.load());
            }
            // Null rather than a division of nothing: `0/0` printed as `nan`, which reads as a
            // measurement and is not one.
            body.raw("bestWordShare", tests == 0 ? "null"
                                                 : JsonBody::real(static_cast<double>(best) /
                                                                  static_cast<double>(tests)));
        } else {
            body.number("addressWord", named);
            body.number("addressWordOffset", static_cast<uint32_t>(named) * 4);
            body.number("addressWordReads", m_wordReads[static_cast<size_t>(named)].load());
        }
        body.raw("addressWordShare", JsonBody::real(kMappedWordShare));
        JsonBody shares;
        for (size_t word = 0; word < kEntryWords; word++) {
            JsonBody one;
            one.number("offset", word * 4);
            one.number("reads", m_wordReads[word].load());
            one.raw("share",
                    JsonBody::real(tests == 0 ? 0.0
                                              : static_cast<double>(m_wordReads[word].load()) /
                                                    static_cast<double>(tests)));
            shares.object(std::to_string(word), one.text());
        }
        body.object("wordReadShares", shares.text());
    }
    body.number("cursorSwitches", m_cursorSwitches);
    body.number("cursorCompared", m_cursorCompared);
    JsonBody examples;
    for (size_t index = 0; index < m_exampleCount; index++) {
        const Binding& binding = m_examples[index];
        JsonBody one;
        one.number("cursor", binding.cursor);
        one.string("object", hex(binding.object));
        one.number("offset", binding.entry[kEntryBlockAddress / 4]);
        one.number("size", binding.entry[kEntryBlockSize / 4]);
        JsonBody words;
        for (size_t word = 0; word < kEntryWords; word++) {
            words.number(std::to_string(word).c_str(), binding.entry[word]);
        }
        one.raw("entry", words.text());
        JsonBody readable;
        for (size_t word = 0; word < kEntryWords; word++) {
            readable.raw(std::to_string(word).c_str(), binding.mapped[word] ? "true" : "false");
        }
        one.raw("readableAsAddress", readable.text());
        // The other slot, whole, because whether the previous tick's values are
        // still in memory when this tick binds is the question a blend rests on
        // and it is answered by reading that slot and not by assuming a ring.
        one.number("otherCursor", binding.otherCursor);
        one.number("otherOffset", binding.otherEntry[kEntryBlockAddress / 4]);
        one.number("otherSize", binding.otherEntry[kEntryBlockSize / 4]);
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
        // Named `address` and not `offset` because there is no base: the decompilation shows
        // the binder handing one of these two words straight to the uniform block register.
        one.number("blockAddress", binding.block);
        one.number("otherBlockAddress", binding.otherBlock);
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
    // The correlation's own coverage and error rate, in the same report as the numbers it
    // qualifies. A reader who has to go and find it cannot check it, and a correlation whose
    // error rate is not beside its results is one that gets believed.
    if (m_scope == nullptr) {
        body.raw("objectIdentity", "null");
    } else {
        body.object("objectIdentity", m_scope->json());
    }
    // The draw attribute census, served here because this is the report a reader already has
    // open when they ask where the position is: three places the pose was looked for, and the
    // attribute table that says where it now is.
    body.object("drawAttributes", m_drawAttributes == nullptr ? "null" : m_drawAttributes->json());
    // And the falsifier: whether two ticks' position bytes exist to be blended at all, per
    // node, with the four answers kept apart. A report that only said "blendable: 3" would not
    // say what happened to the other five.
    body.object("vertexHistory", m_vertexHistory == nullptr ? "null" : m_vertexHistory->json());
    // The objective's second question about the pose: whether tick N-1's uniform block contents
    // are still there when tick N paints. Measured by re-reading the earlier address, so it is
    // in the report rather than in a note beside it.
    body.object("blockRing", m_ring == nullptr ? "null" : m_ring->json());
    body.object("blockAddress", m_address == nullptr ? "null" : m_address->json());
    if (!m_refusal.empty()) {
        body.string("refusal", m_refusal);
    }
    return body.finish();
}

} // namespace wiiuport::title
