#include "check.h"
#include "suites.h"
#include "wiiuport/title/NodePoseLocator.h"
#include "wiiuport/title/UniformBlockCensus.h"

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <vector>

using wiiuport::title::NodePoseLocator;
using wiiuport::title::UniformBlockCensus;

namespace {

// A guest the census can be pointed at: a sparse map of guest words, so a
// refusal to read is a refusal rather than a zero.
class FakeGuest {
  public:
    bool writeWord(uint32_t address, uint32_t value) {
        if (address == 0) {
            return false;
        }
        m_words[address] = value;
        return true;
    }

    bool readWord(uint32_t address, uint32_t& value) const {
        const auto found = m_words.find(address);
        if (found == m_words.end()) {
            return false;
        }
        value = found->second;
        return true;
    }

  private:
    std::map<uint32_t, uint32_t> m_words;
};

FakeGuest* g_fake = nullptr;
GuestCallProbes::Probe* g_first = nullptr;
GuestCallProbes::Probe* g_second = nullptr;

void keepRegistration(uint32_t entry, uint32_t, GuestCallProbes::Probe& probe, bool /*holdsEntry*/,
                      uint32_t /*resume*/) {
    if (entry == UniformBlockCensus::kBinder) {
        g_first = &probe;
    } else {
        g_second = &probe;
    }
}

bool readWord(uint32_t address, uint32_t& value) {
    return g_fake->readWord(address, value);
}

// The pose seam, the same guest read a word at a time: the census's own tests are
// about the descriptor, and the pose history is exercised where it is measured.
bool readWords(uint32_t address, uint32_t* values, uint32_t count) {
    for (uint32_t word = 0; word < count; word++) {
        if (!g_fake->readWord(address + 4 * word, values[word])) {
            return false;
        }
    }
    return true;
}

UniformBlockCensus makeCensus(FakeGuest& guest) {
    g_fake = &guest;
    g_first = nullptr;
    g_second = nullptr;
    return UniformBlockCensus(&keepRegistration, &readWord, &readWords);
}

// One binding, as the draw makes it: the sub-object is the first argument.
void bind(GuestCallProbes::Probe* probe, uint32_t object) {
    std::array<uint32_t, 32> gpr{};
    gpr[3] = object;
    probe->OnCall(std::span<const uint32_t, 32>(gpr.data(), gpr.size()), 0);
}

void linked() {
    g_first->OnInstall(GuestCallProbes::Installation::Installed);
    g_second->OnInstall(GuestCallProbes::Installation::Installed);
}

// A sub-object whose descriptor list has two entries, the second in use.
constexpr uint32_t kObject = 0x43e00000;

// A whole descriptor entry, all seven words: the census reports the entry whole,
// so a fake that filled only the two the binder uses would read as an entry it
// could not read, which is the same report a real unmapped one gives.
void writeEntry(FakeGuest& guest, uint32_t entry, uint32_t offset, uint32_t size) {
    for (uint32_t word = 0; word < UniformBlockCensus::kEntryWords; word++) {
        uint32_t value = 0;
        if (word == UniformBlockCensus::kEntryOffsetOffset / 4) {
            value = offset;
        } else if (word == UniformBlockCensus::kEntrySizeOffset / 4) {
            value = size;
        } else {
            value = 0x1000 + word;
        }
        guest.writeWord(kObject + UniformBlockCensus::kEntriesOffset +
                            entry * UniformBlockCensus::kEntrySize + 4 * word,
                        value);
    }
}

void theOtherSlotIsReadAsWellAsTheBoundOne();
void cursorSwitchesAreCountedAgainstTheBindsTheyCouldBeAmong();
void theTwoAddressesABindingNamesAreBothFed();

// The node locator's own registration, kept apart from the census's two: the census's sink
// decides by address, and the node draw's entry is neither binder, so it would land in the
// second slot and stand in for a probe that is not there.
GuestCallProbes::Probe* g_nodeProbe = nullptr;

void keepNodeRegistration(uint32_t, uint32_t, GuestCallProbes::Probe& probe, bool, uint32_t) {
    g_nodeProbe = &probe;
}

NodePoseLocator makeNodeLocator() {
    g_nodeProbe = nullptr;
    return NodePoseLocator(&keepNodeRegistration, &readWords);
}

} // namespace

void wiiuport::tests::runBlockCensusTests() {
    theTwoAddressesABindingNamesAreBothFed();
    theOtherSlotIsReadAsWellAsTheBoundOne();
    cursorSwitchesAreCountedAgainstTheBindsTheyCouldBeAmong();
    // An object the binder would be handed, with its cursor naming the second
    // entry and both entries filled in.
    FakeGuest guest;
    guest.writeWord(kObject + UniformBlockCensus::kCursorOffset, 1);
    writeEntry(guest, 0, 0x1000, 0x40);
    writeEntry(guest, 1, 0x2000, 0x80);
    // The memory a real uniform block would be in, so "does this word name
    // somewhere readable" has a true answer to give as well as a false one.
    guest.writeWord(0x3000, 0x3f800000);
    guest.writeWord(0x3004, 0x40000000);
    UniformBlockCensus census = makeCensus(guest);
    census.install();
    linked();

    check::isTrue(census.bindings() == 0, "nothing is counted before the binder is called");
    check::isTrue(census.json().find("\"probe\":\"installed\"") != std::string::npos,
                  "and the report says the probe is in place");

    bind(g_first, kObject);
    check::isTrue(census.bindings() == 1, "one binding counted");
    const std::string body = census.json();
    check::isTrue(body.find("\"objects\":1") != std::string::npos,
                  "one object seen, so a histogram is not the title's whole scene");
    check::isTrue(body.find("\"1\":1") != std::string::npos,
                  "and the cursor histogram names the entry that was read");
    check::isTrue(body.find("\"offset\":8192") != std::string::npos,
                  "the report carries the block's offset as read");
    check::isTrue(body.find("\"size\":128") != std::string::npos, "and its size");
    // The block's address is a base the title set elsewhere, so the entry is
    // where it has to be looked for: which words, added to the offset, name
    // memory the guest can read.
    check::isTrue(body.find("\"readableAtOffset\"") != std::string::npos,
                  "and the report says which of the entry's words read at that offset");
    // The fake's words are 0x1000+word, and the block the entry names would sit
    // at that plus the offset -- which only word 0's does, because the fake maps
    // nothing else. So exactly one word reads, and the report says which.
    check::isTrue(body.find("\"readableAtOffset\":{\"0\":true") != std::string::npos,
                  "the one word that names a mapped block is reported as such");
    check::isTrue(body.find("\"3\":false") != std::string::npos,
                  "and the one that does not as not");

    // The other binder counts into the same census: they are the same function
    // apart from which triple of indices they read, so one report covers both.
    bind(g_second, kObject);
    check::isTrue(census.bindings() == 2, "the second binder counts too");

    // A cursor that alternates is the title double buffering; a census that
    // cannot read the object says so instead of reporting a zero.
    guest.writeWord(kObject + UniformBlockCensus::kCursorOffset, 0);
    bind(g_first, kObject);
    bind(g_first, kObject);
    guest.writeWord(kObject + UniformBlockCensus::kCursorOffset, 1);
    bind(g_first, kObject);
    // Two bindings on entry 1 (the first two), then two on entry 0, then one
    // back on entry 1: the histogram is 2 and 3, and what it says is that the
    // title reads both of the two entries its constructor allocated.
    check::isTrue(census.json().find("\"cursors\":{\"0\":2,\"1\":3}") != std::string::npos,
                  "a cursor that moves shows both entries read, and the counts");

    // An object nothing can be read out of: counted, and reported as unread
    // rather than as entry zero.
    FakeGuest empty;
    UniformBlockCensus blind = makeCensus(empty);
    blind.install();
    linked();
    bind(g_first, kObject);
    check::isTrue(blind.bindings() == 1, "a binding on an unreadable object still counts");
    check::isTrue(blind.json().find("\"read\":false") != std::string::npos,
                  "and the report says the cursor could not be read");
    check::isTrue(blind.json().find("\"cursorsOutOfRange\":1") != std::string::npos,
                  "which is counted as out of range rather than as entry zero");

    // A binding with no object at all is counted and does not become a cursor.
    bind(g_first, 0);
    check::isTrue(blind.bindings() == 2, "a binding with no object still counts");
}

namespace {

// The other slot, read whole -- because whether the previous tick's values are
// still in memory when this tick binds is what a blend rests on, and a ring read
// only where the cursor points cannot answer it.
void theOtherSlotIsReadAsWellAsTheBoundOne() {
    FakeGuest guest;
    auto census = makeCensus(guest);
    census.install();
    linked();
    writeEntry(guest, 0, 0x40, 0x200);
    writeEntry(guest, 1, 0x40, 0x300);
    // Make both entries name memory that reads, so both are reported as blocks:
    // the entry's word 0 plus its offset has to be a mapped address.
    // The entries name blocks by putting their word 0 (0x1000) plus their offset
    // (0x40) together, so both land on 0x1040, which is written here and so reads.
    guest.writeWord(0x1000 + 0x40, 0);
    guest.writeWord(0x1000 + 0x40 + 4, 0);

    guest.writeWord(kObject + UniformBlockCensus::kCursorOffset, 1);
    bind(g_first, kObject);
    const std::string body = census.json();
    check::isTrue(body.find("\"cursor\":1") != std::string::npos,
                  "the bound slot is the one the cursor names");
    check::isTrue(body.find("\"size\":768") != std::string::npos,
                  "and its size is that entry's own: 0x300");
    check::isTrue(body.find("\"otherCursor\":0") != std::string::npos,
                  "the slot this binding did not use is named");
    // Entry 0 was written with 0x200, entry 1 with 0x300, so a report that read
    // the bound slot's size for both of them would say 0x300 twice. This is the
    // check that it did not.
    check::isTrue(body.find("\"otherSize\":512") != std::string::npos,
                  "and the other slot carries its own size, not the bound one's");
    check::isTrue(body.find("\"otherOffset\":64") != std::string::npos,
                  "and that slot's own offset");
    check::isTrue(body.find("\"otherEntry\"") != std::string::npos,
                  "and the whole of that entry, word for word");
    check::isTrue(body.find("\"otherReadableAtOffset\"") != std::string::npos,
                  "and which of its words name memory the guest can read");
}

// Whether the ring turns per bind or per frame, counted rather than read, and
// reported with the count it is a fraction of. A title that draws each object once
// would otherwise report no switches at all for a ring that works.
void cursorSwitchesAreCountedAgainstTheBindsTheyCouldBeAmong() {
    FakeGuest guest;
    auto census = makeCensus(guest);
    census.install();
    linked();
    writeEntry(guest, 0, 0x40, 0x200);
    writeEntry(guest, 1, 0x40, 0x200);
    // Four bindings of one object, the cursor moving on the second and the third:
    // the first is not a switch because nothing preceded it for this object, and
    // the fourth is not one either because it did not move.
    const std::array<uint32_t, 4> cursors = {0, 1, 0, 0};
    for (uint32_t cursor : cursors) {
        guest.writeWord(kObject + UniformBlockCensus::kCursorOffset, cursor);
        bind(g_first, kObject);
    }
    const std::string body = census.json();
    check::isTrue(body.find("\"bindings\":4") != std::string::npos, "four bindings were counted");
    check::isTrue(body.find("\"cursorCompared\":3") != std::string::npos,
                  "three of them could be compared, being bindings of an object already seen");
    check::isTrue(body.find("\"cursorSwitches\":2") != std::string::npos,
                  "and two of those moved the cursor");
}

// **The two addresses a binding names are both fed, and scored apart.** A binding's
// argument is the node's sub-object; the node is that one `kSubObjectOffset` away. Four
// sub-objects each hold a rigid transform at 80 bytes and their four nodes hold nothing, so
// a run that shared one table would see 4 of 8, need 5, and name nothing. This is the
// product's real path -- census to locator -- and it is the only place that wiring is
// exercised at all, since the locator's own tests hand it addresses directly.
void theTwoAddressesABindingNamesAreBothFed() {
    NodePoseLocator nodes = makeNodeLocator();
    g_fake = nullptr;
    FakeGuest guest;
    g_fake = &guest;

    // Four sub-objects, each with a transform, each with a node that has none. The whole
    // scanned range is written for both, because a read that stops at the first unmapped
    // word is a refusal, and a fake with a hole in it would give one -- which is the right
    // answer to the wrong question here.
    const std::array<uint32_t, 4> objects = {0x43e10000u, 0x43e20000u, 0x43e30000u, 0x43e40000u};
    // Rigid at every angle, so the pose can *move* between two draws without ceasing to be a
    // transform. Adding a constant to a unit row would do the opposite: the first version of
    // this test did that and the second scan no longer held a transform at all, so the
    // candidate was never compared and nothing was named. A locator that cannot tell "the
    // pose moved" from "the value stopped looking like a pose" is a locator that reports
    // its own test fixture as the title.
    const auto poseAt = [](float spin, uint32_t bits[12]) {
        const float c = std::cos(spin);
        const float s = std::sin(spin);
        const float pose[12] = {c, s, 0.0f, -s, c, 0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 3.0f, 4.0f};
        for (size_t word = 0; word < 12; word++) {
            std::memcpy(&bits[word], &pose[word], sizeof(uint32_t));
        }
    };
    for (uint32_t object : objects) {
        for (uint32_t word = 0; word < NodePoseLocator::kScanWords; word++) {
            guest.writeWord(object + 4 * word, 0);
            guest.writeWord(object - NodePoseLocator::kSubObjectOffset + 4 * word, 0);
        }
        uint32_t bits[12] = {};
        poseAt(0.0f, bits);
        for (size_t word = 0; word < 12; word++) {
            guest.writeWord(object + 20 * 4 + 4 * word, bits[word]);
        }
    }

    UniformBlockCensus census(&keepRegistration, &readWord, &readWords, nullptr, &nodes);
    census.install();
    linked();

    // Two binds each, so an object is scanned more than once and a value that does not
    // move can be told from one that does.
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t object : objects) {
            guest.writeWord(object + UniformBlockCensus::kCursorOffset, 0);
            uint32_t bits[12] = {};
            poseAt(0.2f * static_cast<float>(pass + 1), bits);
            for (size_t word = 0; word < 12; word++) {
                guest.writeWord(object + 20 * 4 + 4 * word, bits[word]);
            }
            bind(g_first, object);
        }
    }

    const std::string body = census.json();
    const size_t at = body.find("\"nodePose\":{");
    check::isTrue(at != std::string::npos, "the census carries the node pose locator: " + body);
    const std::string node = body.substr(at);
    check::isTrue(node.find("\"objectsTracked\":4") != std::string::npos,
                  "and all four nodes were fed, the node being a fixed subtraction off the "
                  "binding's argument");
    check::isTrue(node.find("\"bestOffset\":80") != std::string::npos,
                  "and the sub-object's field is named, 20 floats in: " + node);
}

} // namespace
