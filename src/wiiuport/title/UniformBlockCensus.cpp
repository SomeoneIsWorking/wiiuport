#include "wiiuport/title/UniformBlockCensus.h"

#include "wiiuport/guest/ProbeInstallation.h"
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
    // r3 is the sub-object whose descriptor the binder walks; **r4 is the material it binds it
    // for**, and the block *index* lives in there rather than in the record. The binder reads
    // `iVar3 = *(int *)(material + 0x10) + 0x28` when `*(uint *)(material + 0xc) > 2`, and the
    // vertex, pixel and geometry indices are the shorts at `iVar3 + 0xc`, `+0xe` and `+0x10`.
    // Handed over as read, because whether those shorts are register indices is a measurement
    // and not a reading of the decompilation.
    m_owner.readMaterial(gpr[4]);
    m_owner.record(gpr[3], m_second);
}

void UniformBlockCensus::readMaterial(uint32_t material) {
    if (material == 0) {
        m_materialUnread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The two words that decide the base, and the three shorts off it. Read as the binder reads
    // them: a count first, and the base only when the count says there is more than one entry.
    uint32_t count = 0;
    if (!m_readWord(material + 0x0c, count)) {
        m_materialUnread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint32_t offset = 0;
    if (!m_readWord(material + 0x10, offset)) {
        m_materialUnread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t base = count > 2 ? offset + 0x28 : 0u;
    Material read;
    read.material = material;
    read.count = count;
    read.offset = offset;
    read.base = base;
    bool complete = true;
    const uint32_t at = base + 0x0c;
    complete = m_readWord(at, read.raw0) && complete;
    complete = m_readWord(at + 4, read.raw1) && complete;
    complete = m_readWord(at + 8, read.raw2) && complete;
    if (!complete) {
        m_materialUnread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The three shorts the binder compares against -1, read as the low half of the three words.
    // Signed, because -1 is the "this stage has no block" value and an unsigned 0xffffffff would
    // read as a large index rather than as the absence.
    read.vertexIndex = static_cast<int16_t>(read.raw0 & 0xffffu);
    read.pixelIndex = static_cast<int16_t>(read.raw1 & 0xffffu);
    read.geometryIndex = static_cast<int16_t>(read.raw2 & 0xffffu);
    {
        std::scoped_lock lock(m_mutex);
        if (m_materials.size() < kMaxMaterials) {
            m_materials.push_back(read);
        } else {
            m_materialsRefused.fetch_add(1, std::memory_order_relaxed);
        }
        ++m_materialsRead;
    }
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

void UniformBlockCensus::setIdentity(CommandStreamIdentity* identity) {
    m_identity = identity;
}

void UniformBlockCensus::record(uint32_t object, bool second) {
    // Recorded at the write position before this binder emits its packets.
    if (m_identity != nullptr) {
        m_identity->bind(object);
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
        // The ring, which needs a *place* to re-read, and it is given the title's own two words.
        //
        // Not the mapping route's answer: that route refuses, correctly, because five of the
        // record's seven words read as guest memory and it cannot tell which is meant. It is given
        // `blockOf`, which is the pair the binder passes to `GX2Set*UniformBlock` -- the address
        // and the size, straight from the record, with no base and no host interpretation between.
        if (m_ring != nullptr) {
            uint32_t sizeInBytes = 0;
            const uint32_t address = blockOf(binding.entry, sizeInBytes);
            if (address != 0 && sizeInBytes != 0) {
                m_ring->bind(object, UniformBlockRing::Block{address, sizeInBytes});
            } else {
                m_ring->bindSize(object, binding.entry[kEntryBlockSize / 4]);
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
        mapWords(binding.otherEntry, binding.otherMapped);
        binding.block = blockOf(binding.entry, binding.blockSize);
        binding.otherBlock = blockOf(binding.otherEntry, binding.otherBlockSize);
        // What the bound block holds, read now, while the binder is about to hand it to the GPU.
        //
        // **Given `blockOf`, which is the same address the ring gets -- and which is the fix for a
        // scan that was reading the wrong place entirely.** It used to be handed the record's size
        // word, `0x40`, as though it were an address, so every one of its readings was 64 bytes of
        // guest memory at `0x40` -- one location, read over and over, which is how 233 whole-block
        // scans came to agree. The address is the record's other word, the one the binder passes
        // to `GX2Set*UniformBlock`, and the two are now the same value the ring re-reads.
        //
        // The other slot's address is zero when its record did not read, and a zero is counted as
        // a descriptor that named nothing rather than read as a pose of zeroes. Read before the
        // lock, because a binding on the display thread must not queue behind a report being
        // written.
        m_poseHistory.observe(object, binding.block, binding.otherBlock);
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
    // **The binder's own two words, in the binder's own order, and nothing else.**
    //
    // The decompilation is `GX2SetVertexUniformBlock(iVar5, uVar4, uVar6)` with
    // `uVar4 = entry[0x0c/4]` and `uVar6 = entry[0x04/4]`, so the record supplies the address and
    // the size directly and **no base is added anywhere** -- which is what removed the relative-
    // offset reading and the base hunt with it.
    //
    // Which word is the address is left to the record rather than to a host decision. The
    // documented GX2 order is (index, address, size), so `+0x0c` is the address; the fork's export
    // maps its `gpr[4]` to its own `size` and its `gpr[5]` to its `virtualAddress`, the reverse.
    // **Both words read 0x40 for every object measured**, so the two readings differ in name and
    // not in value, and this returns the title's pair as the title passes it rather than picking a
    // winner. The size is the word that is 64 bytes whichever way round they are read, and the
    // address is the other, and 0x40 is what the register ends up holding either way.
    size = entry[kEntryBlockSize / 4];
    return entry[kEntryBlockAddress / 4];
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
    // route has no answer to give and says so.
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
    body.string("probe", std::string(guest::installationName(m_first.installation)));
    body.string("probeSecond", std::string(guest::installationName(m_second.installation)));
    body.number("entries", kEntries);
    body.number("bindings", m_bindings.load());
    body.number("objects", m_objects);
    JsonBody cursors;
    for (int entry = 0; entry < kEntries; entry++) {
        cursors.number(std::to_string(entry).c_str(), m_cursors[entry]);
    }
    body.raw("cursors", cursors.text());
    body.number("cursorsOutOfRange", m_cursorsOutOfRange);
    // The material, and the block indices inside it. This is where the title names its block: not
    // by address and not by the record, but by a register index read out of r4.
    body.number("materialsRead", m_materialsRead.load());
    body.number("materialsUnread", m_materialUnread.load());
    body.number("materialsRefused", m_materialsRefused.load());
    body.number("materialsDistinct", m_materials.size());
    {
        // **All three stages, because the empty one is not the answer.** The first version
        // histogrammed only the vertex stage and reported "0 materials carry a vertex block
        // index" -- true, and beside a geometry index of 3 sitting in the same record. -1 is the
        // title's "this stage has no block", so a stage with none is an absence to count, not a
        // stage to leave out.
        //
        // Signed, and printed signed: `JsonBody::number` takes a `uint64_t`, so a `-1` came out as
        // 18446744073709551615 -- which reads as an enormous index and is precisely the value the
        // decompilation says means "no block".
        struct Stage {
            const char* name;
            int (*of)(const Material&);
        };

        const Stage stages[] = {
            {"vertex",
             [](const Material& one) {
                 return one.vertexIndex;
             }},
            {"pixel",
             [](const Material& one) {
                 return one.pixelIndex;
             }},
            {"geometry",
             [](const Material& one) {
                 return one.geometryIndex;
             }},
        };
        JsonBody byStage;
        for (const Stage& stage : stages) {
            uint64_t carrying = 0;
            uint64_t absent = 0;
            std::map<int, uint64_t> indices;
            for (const Material& one : m_materials) {
                const int index = stage.of(one);
                if (index >= 0) {
                    carrying++;
                    indices[index]++;
                } else {
                    absent++;
                }
            }
            JsonBody one;
            one.number("carrying", carrying);
            one.number("absent", absent);
            one.number("distinct", indices.size());
            // The claim is "small integers", and "small" needs the largest one stated. An index
            // that were an address or a pointer would be enormous here, and that is the falsifier.
            one.raw("largest", indices.empty() ? "null" : std::to_string(indices.rbegin()->first));
            one.raw("smallest", indices.empty() ? "null" : std::to_string(indices.begin()->first));
            JsonBody listed;
            size_t shown = 0;
            for (const auto& [index, seen] : indices) {
                if (shown >= kExamples) {
                    break;
                }
                JsonBody entry;
                entry.number("index", index);
                entry.number("materials", seen);
                listed.object(std::to_string(shown), entry.text());
                shown++;
            }
            one.object("byValue", listed.text());
            byStage.object(stage.name, one.text());
        }
        body.object("blockIndicesByStage", byStage.text());
        JsonBody sample;
        for (size_t index = 0; index < m_materials.size() && index < kExamples; index++) {
            JsonBody one;
            one.string("material", hex(m_materials[index].material));
            one.number("count", m_materials[index].count);
            one.number("offset", m_materials[index].offset);
            one.number("base", m_materials[index].base);
            one.raw("vertexIndex", std::to_string(m_materials[index].vertexIndex));
            one.raw("pixelIndex", std::to_string(m_materials[index].pixelIndex));
            one.raw("geometryIndex", std::to_string(m_materials[index].geometryIndex));
            sample.object(std::to_string(index), one.text());
        }
        body.object("sampleMaterials", sample.text());
    }
    body.number("otherRecordsRead", m_otherRecordsRead.load());
    body.number("otherRecordsUnread", m_otherRecordsUnread.load());
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
            body.number("addressWordOffset", static_cast<uint64_t>(named) * 4u);
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
    if (m_identity == nullptr) {
        body.raw("objectIdentity", "null");
    } else {
        body.object("objectIdentity", m_identity->json());
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
    if (!m_refusal.empty()) {
        body.string("refusal", m_refusal);
    }
    return body.finish();
}

} // namespace wiiuport::title
