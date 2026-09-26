#include "check.h"
#include "suites.h"
#include "wiiuport/title/UniformBlockCensus.h"

#include <array>
#include <map>
#include <span>
#include <string>
#include <vector>

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

void keepRegistration(uint32_t entry, uint32_t, GuestCallProbes::Probe& probe) {
    if (entry == UniformBlockCensus::kBinder) {
        g_first = &probe;
    } else {
        g_second = &probe;
    }
}

bool readWord(uint32_t address, uint32_t& value) {
    return g_fake->readWord(address, value);
}

UniformBlockCensus makeCensus(FakeGuest& guest) {
    g_fake = &guest;
    g_first = nullptr;
    g_second = nullptr;
    return UniformBlockCensus(&keepRegistration, &readWord);
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

} // namespace

void wiiuport::tests::runBlockCensusTests() {
    // An object the binder would be handed, with its cursor naming the second
    // entry and both entries filled in.
    FakeGuest guest;
    guest.writeWord(kObject + UniformBlockCensus::kCursorOffset, 1);
    writeEntry(guest, 0, 0x1000, 0x40);
    writeEntry(guest, 1, 0x2000, 0x80);
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
