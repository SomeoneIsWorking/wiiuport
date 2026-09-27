#include "check.h"
#include "suites.h"
#include "wiiuport/title/LogicGate.h"

#include <algorithm>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

using wiiuport::title::LogicGate;

namespace {

// A guest the gate can be pointed at: a sparse map of guest words, so a refusal
// to read is a refusal rather than a zero.
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

constexpr uint32_t kBlock = 0x00e07000;

FakeGuest* g_fake = nullptr;
GuestCallProbes::Probe* g_probe = nullptr;

// Whether the registration asked to keep the entry. It must not: a probe that
// holds the tick's entry takes it from the caller census for the rest of the run,
// and the only symptom is a count of zero that reads as a title that never ticks.
// Every registration the gate makes, with what it asked for. The tick's must be
// momentary and the gate's own block's must not: the first because holding the
// tick's entry takes it from the caller census for the rest of the run, the second
// because the count of entries is the whole diagnosis.
std::vector<std::pair<uint32_t, bool>> g_registrations;

void keepRegistration(uint32_t entry, uint32_t, GuestCallProbes::Probe& probe, bool holdsEntry) {
    g_probe = &probe;
    g_registrations.emplace_back(entry, holdsEntry);
}

// Whether the registration for `entry` asked to keep it. A registration that is
// not there at all counts as keeping it, so a missing registration fails the
// check rather than passing it.
bool holdsEntry(uint32_t entry) {
    const auto found =
        std::find_if(g_registrations.begin(), g_registrations.end(), [entry](const auto& pair) {
            return pair.first == entry;
        });
    return found != g_registrations.end() ? found->second : true;
}

uint32_t allocateCode(uint32_t) {
    return kBlock;
}

// The counters' block: a different address from the code block, because it is a
// different kind of memory. The guest writes this one and only executes the
// other, and the gate's whole difficulty was that it could not rely on a store
// into the area meant for instructions.
constexpr uint32_t kCounters = 0x00e0a000;

uint32_t allocateData(uint32_t) {
    return kCounters;
}

bool writeWord(uint32_t address, uint32_t value) {
    return g_fake->writeWord(address, value);
}

bool readWord(uint32_t address, uint32_t& value) {
    return g_fake->readWord(address, value);
}

LogicGate makeGate(FakeGuest& guest) {
    g_fake = &guest;
    g_probe = nullptr;
    g_registrations.clear();
    return LogicGate(&keepRegistration, &allocateCode, &allocateData, &writeWord, &readWord);
}

// The moment the fork reports the title is linked, which is where the gate's
// memory comes from. The word after the entry is the tick's own second
// instruction -- `stw r0,0x4(r1)` at 0x025d42f0 in the title's image -- and that
// is the word the gate takes for its branch, so that is what is here.
void linked() {
    g_probe->OnInstall(GuestCallProbes::Installation::Installed);
    g_fake->writeWord(LogicGate::kTickBody, LogicGate::kTickSecond);
}

} // namespace

// The report is read by a JSON parser and by nothing else, so "it has the field"
// is not the claim: "it parses" is. One unquoted word -- which `raw` writes
// verbatim -- made the whole body unreadable, and every caller fell back to a
// probe the gate holds, so the simulation was reported at zero hertz while it was
// running at thirty. The check is a parse, not a substring.
void theReportParsesAsJson() {
    FakeGuest guest;
    LogicGate gate = makeGate(guest);
    gate.install();
    linked();
    // The entry holds the probe's own branch, which is what the report is for:
    // it says what the tick is being entered through.
    guest.writeWord(LogicGate::kTick, 0x4a831598);
    check::isTrue(gate.enable().empty(), "the gate installs for the report to describe");
    const std::string body = gate.json();
    // Every value is either quoted, a number, true, false or null. A bare `0x`
    // is the failure this is here for, so it is looked for directly as well as
    // through the pairing of quotes and colons.
    check::isTrue(body.find("\"wordAtTickBody\":\"0x") != std::string::npos,
                  "the word at the tick's body is quoted");
    check::isTrue(body.find("\"wordAtTickEntry\":\"0x") != std::string::npos,
                  "and so is the word at the tick's entry");
    check::isTrue(body.find(":0x") == std::string::npos, "no value is left unquoted");
    check::isTrue(body.front() == '{' && body.find("}\n") != std::string::npos,
                  "and the body is one object that ends");
}

void wiiuport::tests::runLogicGateTests() {
    theReportParsesAsJson();
    // The gate's words, one at a time, because every one is lifted from the
    // title and a payload this size is mostly the cost of being sure.
    {
        const auto words = LogicGate::payload(kBlock, kCounters);
        check::isTrue(words.size() == LogicGate::kGateWords, "the gate is sixteen words");
        if (words.size() != LogicGate::kGateWords) {
            return;
        }
        // In the data block, which is where the payload counts and where the
        // report says the counters are.
        const uint32_t calls = kCounters + 4 * LogicGate::kCallsWord;
        const uint32_t ticks = kCounters + 4 * LogicGate::kTicksWord;
        const uint32_t through = kBlock + 4 * LogicGate::kThroughWord;
        const uint32_t tail = through + 8;
        // Every word below is written as opcode, then source in bits 21-25, then
        // destination in bits 16-20, then the immediate -- the order the
        // encodings actually have. The two that were got wrong first time are
        // the two whose second field is easy to type as the first, and both were
        // checked against an instruction in the title's own image:
        //   ori r0,r0,0x7431  0x60007431  (from the paint mod's payload work)
        //   addi r1,r1,0x8     0x38210008  (the tick's own epilogue, word 12)
        check::isTrue(words[0] == (0x3c000000u | (3u << 21) | ((calls >> 16) & 0xffff)),
                      "it starts by naming the call counter: lis r3");
        check::isTrue(words[1] == (0x60000000u | (3u << 21) | (3u << 16) | (calls & 0xffff)),
                      "and loading its low half into r3 and not into r0: ori r3,r3");
        check::isTrue(words[2] == (0x80000000u | (4u << 21) | (3u << 16)),
                      "then reading it: lwz r4,0(r3)");
        check::isTrue(words[3] == (0x38000000u | (4u << 21) | (4u << 16) | 1u),
                      "and adding one to what it read: addi r4,r4,1");
        check::isTrue(words[4] == (0x90000000u | (4u << 21) | (3u << 16)),
                      "storing it back: stw r4,0(r3)");
        check::isTrue(words[5] == (0x70000000u | (4u << 21) | (4u << 16) | 1u),
                      "and testing one bit of it: andi. r4,r4,1");
        check::isTrue(words[6] == (0x40800000u | ((through - (kBlock + 4 * 6)) & 0xfffc)),
                      "an odd call branches over the tick");
        check::isTrue(words[7] == (0x3c000000u | (5u << 21) | ((ticks >> 16) & 0xffff)),
                      "an even one counts the tick it lets through: lis r5");
        check::isTrue(words[8] == (0x60000000u | (5u << 21) | (5u << 16) | (ticks & 0xffff)),
                      "and loads its low half into r5: ori r5,r5");
        check::isTrue(words[9] == (0x80000000u | (6u << 21) | (5u << 16)),
                      "reads it: lwz r6,0(r5)");
        check::isTrue(words[10] == (0x38000000u | (6u << 21) | (6u << 16) | 1u),
                      "adds one: addi r6,r6,1");
        check::isTrue(words[11] == (0x90000000u | (6u << 21) | (5u << 16)),
                      "stores it: stw r6,0(r5)");
        // A skipped call returns with the link register as the caller left it:
        // the gate has not touched r0 or r1 on this path.
        check::isTrue(words[12] == 0x4e800020, "and a skipped call returns: blr");
        // The through path supplies the tick's own first two instructions, lifted
        // whole from 0x025d42ec and 0x025d42f0 of the title's image, because the
        // tick cannot be entered past them: its epilogue reads the saved link
        // register back out of the caller's frame and returns through it.
        check::isTrue(words[13] == 0x7c0802a6, "a call it lets through saves LR as the title does");
        check::isTrue(words[14] == 0x90010004,
                      "and stores it in the caller's frame as the title does");
        // Then a tail branch, so the tick's own return goes to the title's caller
        // and not back into this memory: 0x025d42f4 less 0x00e07038.
        check::isTrue(words[15] == (18u << 26) | ((LogicGate::kTickBody + 4 - tail) & 0x03fffffcu),
                      "and the tick is branched at, not called: 0x025d42f4");
    }
    // A block the tick and the gate cannot both reach is refused rather than
    // written with a displacement that lands elsewhere.
    {
        check::isTrue(LogicGate::payload(0x40000000, kCounters).empty(),
                      "a gate too far from the tick is refused");
    }
    // Installing, and putting it back.
    {
        FakeGuest guest;
        LogicGate gate = makeGate(guest);
        gate.install();
        linked();
        check::isTrue(!holdsEntry(LogicGate::kTick),
                      "the gate's probe on the tick asks not to keep the entry, so the caller "
                      "census on that address can still take it");
        check::isTrue(holdsEntry(kBlock),
                      "and the probe on the gate's own block does keep it, because counting the "
                      "gate's entries is what says whether the gate is reached at all");
        check::isTrue(gate.enable().empty(), "the gate installs over the tick's body");
        uint32_t word = 0;
        check::isTrue(readWord(LogicGate::kTickBody, word) && word != LogicGate::kTickSecond,
                      "and the tick's body now branches somewhere else");
        check::isTrue(gate.enabled(), "the report says it is in");
        check::isTrue(gate.disable().empty(), "and it comes back out");
        check::isTrue(readWord(LogicGate::kTickBody, word) && word == LogicGate::kTickSecond,
                      "with the tick's own second instruction back");
        check::isTrue(!gate.enabled(), "and the report says it is out");
        check::isTrue(gate.disable().empty(), "and disabling twice is not a refusal");
    }
    // A tick that starts differently is refused by name, and the gate is not in.
    {
        FakeGuest guest;
        LogicGate gate = makeGate(guest);
        gate.install();
        g_probe->OnInstall(GuestCallProbes::Installation::Installed);
        guest.writeWord(LogicGate::kTickBody, 0x60000000);
        const std::string refusal = gate.enable();
        check::isTrue(refusal.find("this gate is written for this title's tick") !=
                          std::string::npos,
                      "a foreign tick is refused by name");
        check::isTrue(!gate.enabled(), "and nothing is installed");
    }
    // The counters are the report's measurement, so both words are named.
    {
        FakeGuest guest;
        LogicGate gate = makeGate(guest);
        gate.install();
        linked();
        gate.enable();
        // A counter the arena has never held is reported as unread, not as
        // zero: a rate is taken from two readings, and a zero standing in for
        // "unknown" would be a number the report invented.
        std::string body = gate.json();
        check::isTrue(body.find("\"enabled\":true") != std::string::npos, "and it is in");
        check::isTrue(body.find("\"callsCount\":null") != std::string::npos,
                      "the call counter reads as unread before any call");
        // Written, it reads.
        guest.writeWord(kCounters + 4 * LogicGate::kCallsWord, 41);
        guest.writeWord(kCounters + 4 * LogicGate::kTicksWord, 20);
        body = gate.json();
        check::isTrue(body.find("\"callsCount\":41") != std::string::npos,
                      "and the call counter reads what is there");
        check::isTrue(body.find("\"ticksCount\":20") != std::string::npos,
                      "and so does the tick counter");
    }
}
