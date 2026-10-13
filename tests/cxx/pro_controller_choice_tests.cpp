#include "check.h"
#include "suites.h"
#include "wiiuport/title/ProControllerChoice.h"

#include <map>
#include <string>
#include <vector>

using wiiuport::title::ProControllerChoice;

namespace {

constexpr uint32_t kBlock = 0x00e08000;

struct Registration {
    uint32_t entry;
    GuestCallProbes::Probe* probe;
    bool holdsEntry;
    uint32_t resume;
};

std::vector<Registration> g_registrations;
std::map<uint32_t, uint32_t> g_words;
uint32_t g_arena = kBlock;

void keepRegistration(uint32_t entry, uint32_t, GuestCallProbes::Probe& probe, bool holdsEntry,
                      uint32_t resume) {
    g_registrations.push_back({entry, &probe, holdsEntry, resume});
}

uint32_t allocateCode(uint32_t) {
    return g_arena;
}

bool writeWord(uint32_t address, uint32_t value) {
    g_words[address] = value;
    return true;
}

// Where the relative branch at block word `index` lands; `mask` is its displacement field.
uint32_t landing(const std::vector<uint32_t>& words, size_t index, uint32_t mask) {
    uint32_t displacement = words[index] & mask;
    if ((displacement & ((mask + 4) >> 1)) != 0) {
        displacement |= ~mask;
    }
    return kBlock + 4 * static_cast<uint32_t>(index) + displacement;
}

// The address the `lis`/`ori` pair at block word `index` loads.
uint32_t loadedAddress(const std::vector<uint32_t>& words, size_t index) {
    return ((words[index] & 0xffff) << 16) | (words[index + 1] & 0xffff);
}

void theBlockRunsTheProPath() {
    const auto words = ProControllerChoice::payload(kBlock);
    check::isTrue(words.size() == ProControllerChoice::kBlockWords, "the block is 29 words");
    if (words.size() != ProControllerChoice::kBlockWords) {
        return;
    }
    check::isTrue(words[0] == 0xbe810058 && words[1] == 0x7c0802a6 && words[2] == 0x9001008c,
                  "it saves what the exec's prologue saves, in the frame the stub opened");
    check::isTrue(words[3] == 0x7c7c1b78, "and keeps the screen in r28");
    check::isTrue(loadedAddress(words, 4) == ProControllerChoice::kProControllerReady &&
                      words[6] == 0x7d8903a6 && words[7] == 0x4e800421,
                  "it asks FUN_026d43dc whether a Pro Controller is there");
    check::isTrue(words[8] == 0x2c030000 && (words[9] & 0xffff0003) == 0x41820000,
                  "and branches on a zero answer");
    check::isTrue(landing(words, 9, 0xfffc) == kBlock + 4 * 24, "to the not-ready tail");
    check::isTrue(words[10] == 0x3b000001 && words[11] == 0x931c0054,
                  "otherwise the screen's button becomes 1, the Pro Controller");
    check::isTrue(words[12] == 0x387c0018 &&
                      loadedAddress(words, 13) == ProControllerChoice::kOutAll,
                  "and its state machine is asked for StateID_OutAll");
    check::isTrue(loadedAddress(words, 15) == ProControllerChoice::kChangeState &&
                      words[17] == 0x7d8903a6 && words[18] == 0x4e800421,
                  "through changeState");
    check::isTrue(words[19] == 0xba810058 && words[20] == 0x8001008c && words[21] == 0x7c0803a6 &&
                      words[22] == 0x38210088 && words[23] == 0x4e800020,
                  "then returns through the exec's own epilogue");
    check::isTrue(words[24] == 0x7f83e378 && words[25] == 0x8001008c && words[26] == 0x7c0803a6 &&
                      words[27] == 0xba810058,
                  "not ready, it restores the screen, the link and the saved registers");
    check::isTrue((words[28] & 0xfc000003) == 0x48000000 &&
                      landing(words, 28, 0x03fffffc) == ProControllerChoice::kDisplayBody,
                  "and continues at the exec's second instruction");
    check::isTrue(ProControllerChoice::payload(0x7e000000).empty(),
                  "a block out of the exec's reach has no payload");
}

void linkingProbesTheExec() {
    g_registrations.clear();
    g_words.clear();
    g_arena = kBlock;
    ProControllerChoice choice({.registerProbe = &keepRegistration,
                                .allocateCode = &allocateCode,
                                .writeWord = &writeWord});
    choice.install();
    check::isTrue(g_registrations.size() == 1 && !g_registrations[0].holdsEntry &&
                      g_registrations[0].entry == ProControllerChoice::kDisplay,
                  "install asks for a momentary probe at the Display exec");
    if (g_registrations.size() != 1) {
        return;
    }
    g_registrations[0].probe->OnInstall(GuestCallProbes::Installation::Installed);
    check::isTrue(g_words.size() == ProControllerChoice::kBlockWords &&
                      g_words[kBlock + 4 * 28] == ProControllerChoice::payload(kBlock)[28],
                  "linking writes the block");
    check::isTrue(g_registrations.size() == 2 && g_registrations[1].holdsEntry &&
                      g_registrations[1].resume == kBlock &&
                      g_registrations[1].entry == ProControllerChoice::kDisplay,
                  "then sends the exec's calls to it");
    if (g_registrations.size() != 2) {
        return;
    }
    g_registrations[1].probe->OnInstall(GuestCallProbes::Installation::Installed);
    const std::string body = choice.json();
    check::isTrue(body.find("\"probe\":\"installed\"") != std::string::npos &&
                      body.find("\"refusal\":\"\"") != std::string::npos,
                  "and the report says the probe holds it");
}

void anUnreachableBlockIsRefused() {
    g_registrations.clear();
    g_words.clear();
    g_arena = 0x7e000000;
    ProControllerChoice choice({.registerProbe = &keepRegistration,
                                .allocateCode = &allocateCode,
                                .writeWord = &writeWord});
    choice.install();
    g_registrations[0].probe->OnInstall(GuestCallProbes::Installation::Installed);
    check::isTrue(g_registrations.size() == 1 && g_words.empty(),
                  "a block that cannot reach the exec is neither written nor probed");
    check::isTrue(choice.json().find("cannot reach") != std::string::npos,
                  "and the report says why");
}

} // namespace

void wiiuport::tests::runProControllerChoiceTests() {
    theBlockRunsTheProPath();
    linkingProbesTheExec();
    anUnreachableBlockIsRefused();
}
