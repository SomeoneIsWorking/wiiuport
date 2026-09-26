#include "check.h"
#include "suites.h"
#include "wiiuport/title/LogicGate.h"

#include <map>
#include <span>
#include <string>
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

void keepRegistration(uint32_t, uint32_t, GuestCallProbes::Probe& probe) {
    g_probe = &probe;
}

uint32_t allocateCode(uint32_t) {
    return kBlock;
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
    return LogicGate(&keepRegistration, &allocateCode, &writeWord, &readWord);
}

// The moment the fork reports the title is linked, which is where the gate's
// memory comes from and where the probe has left the tick's own first
// instruction at the word after the entry.
void linked() {
    g_probe->OnInstall(GuestCallProbes::Installation::Installed);
    g_fake->writeWord(LogicGate::kTickBody, LogicGate::kTickFirst);
}

} // namespace

void wiiuport::tests::runLogicGateTests() {
    // The gate's words, one at a time, because every one is lifted from the
    // title and a payload this size is mostly the cost of being sure.
    {
        const auto words = LogicGate::payload(kBlock);
        check::isTrue(words.size() == LogicGate::kGateWords, "the gate is fourteen words");
        if (words.size() != LogicGate::kGateWords) {
            return;
        }
        const uint32_t calls = kBlock + 4 * LogicGate::kCallsWord;
        const uint32_t ticks = kBlock + 4 * LogicGate::kTicksWord;
        check::isTrue(words[0] == (0x3c600000u | ((calls >> 16) & 0xffff)),
                      "it starts by naming the call counter");
        check::isTrue(words[1] == (0x60600000u | (calls & 0xffff)), "and loading its low half");
        check::isTrue(words[2] == 0x80830000, "then counting it: lwz r4,0(r3)");
        check::isTrue(words[3] == 0x38800001, "adding one");
        check::isTrue(words[4] == 0x90830000, "and putting it back");
        check::isTrue(words[5] == 0x70840001, "then testing the low bit: andi. r4,r4,1");
        // The skip is a plain relative bne, and it lands on the blr.
        const uint32_t skipTarget = kBlock + 4 * 13;
        check::isTrue(words[6] == (0x40800000u | ((skipTarget - (kBlock + 4 * 6)) & 0xfffc)),
                      "an odd call branches over the tick");
        check::isTrue(words[7] == (0x3ca00000u | ((ticks >> 16) & 0xffff)),
                      "an even one counts the tick it lets through");
        check::isTrue(words[8] == (0x60a00000u | (ticks & 0xffff)), "and loads its low half");
        check::isTrue(words[11] == 0x90c50000, "stores it: stw r6,0(r5)");
        // The tick is reached by a tail branch, so its return goes to the title's
        // caller and not back into this memory.
        // A tail branch, so the tick's own return reaches the title's caller:
        // 0x025d42f4 less 0x00e07030 is 0x017cd2c4, and a primary branch is
        // opcode 18 with that in its low bits.
        check::isTrue(words[12] == 0x497cd2c4u,
                      "the tick is branched at, not called: 0x025d42f4 from 0x00e07030");
        check::isTrue(words[13] == 0x4e800020, "and a skipped call returns");
    }
    // A block the tick and the gate cannot both reach is refused rather than
    // written with a displacement that lands elsewhere.
    {
        check::isTrue(LogicGate::payload(0x40000000).empty(),
                      "a gate too far from the tick is refused");
    }
    // Installing, and putting it back.
    {
        FakeGuest guest;
        LogicGate gate = makeGate(guest);
        gate.install();
        linked();
        check::isTrue(gate.enable().empty(), "the gate installs over the tick's body");
        uint32_t word = 0;
        check::isTrue(readWord(LogicGate::kTickBody, word) && word != LogicGate::kTickFirst,
                      "and the tick's body now branches somewhere else");
        check::isTrue(gate.enabled(), "the report says it is in");
        check::isTrue(gate.disable().empty(), "and it comes back out");
        check::isTrue(readWord(LogicGate::kTickBody, word) && word == LogicGate::kTickFirst,
                      "with the tick's own first instruction back");
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
        guest.writeWord(kBlock + 4 * LogicGate::kCallsWord, 41);
        guest.writeWord(kBlock + 4 * LogicGate::kTicksWord, 20);
        body = gate.json();
        check::isTrue(body.find("\"callsCount\":41") != std::string::npos,
                      "and the call counter reads what is there");
        check::isTrue(body.find("\"ticksCount\":20") != std::string::npos,
                      "and so does the tick counter");
    }
}
