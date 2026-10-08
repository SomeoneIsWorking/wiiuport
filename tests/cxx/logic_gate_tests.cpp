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
uint32_t g_resume = 0;

void keepRegistration(uint32_t entry, uint32_t, GuestCallProbes::Probe& probe, bool holdsEntry,
                      uint32_t resume) {
    g_probe = &probe;
    g_registrations.emplace_back(entry, holdsEntry);
    if (holdsEntry && resume != 0) {
        g_resume = resume;
    }
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
    g_resume = 0;
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

// **Where the four counters live, in words.** Each is a named constant computed from another one,
// and a constant expression written in a function body is a named constant in the wrong place: the
// value does not change, the *name* is what a reader looks for, and four of them spelled out at
// their uses were four places to look.
constexpr uint32_t kCallsAt = kCounters + 4 * LogicGate::kCallsWord;
constexpr uint32_t kTicksAt = kCounters + 4 * LogicGate::kTicksWord;
constexpr uint32_t kThroughAt = kBlock + 4 * LogicGate::kThroughWord;
constexpr uint32_t kBranchAt = kBlock + 4 * LogicGate::kBranchWord;
// The bare pass-through a freshly allocated gate block holds: a branch to the tick's own body, by
// the displacement from the block rather than an absolute address, because the block's address is
// chosen at link time.
constexpr uint32_t kPassThroughAt = (18u << 26) | ((LogicGate::kTickBody - kBlock) & 0x03fffffcu);

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
        check::isTrue(words.size() == LogicGate::kGateWords, "the gate is fourteen words");
        if (words.size() != LogicGate::kGateWords) {
            return;
        }
        // In the data block, which is where the payload counts and where the
        // report says the counters are.
        const uint32_t calls = kCallsAt;
        const uint32_t ticks = kTicksAt;
        const uint32_t through = kThroughAt;
        const uint32_t branchAt = kBranchAt;
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
        // The conditional branch is checked by its BO and BI fields, not by
        // comparing against the constant the code uses. A conditional branch is
        // one opcode with its condition in those two fields, and an expectation
        // that reuses the code's own constant cannot catch the constant being
        // wrong -- which is how `bne` sat here as 0x40800000 (BO=4, BI=0) while
        // the comment beside it named 0x40820014 as the instruction it came from.
        // BO=4 branches when the bit BI names is false, and `andi.` sets BI=2
        // (bit 2, "not equal") when its result is not zero; so `bne` is BO=4 BI=2,
        // and the title's own `bne` at 0x025d4398, 0x025d4678, 0x025d46a4 and
        // 0x0274c964 all carry 0x4082....
        {
            const uint32_t branch = words[6];
            const uint32_t opcode = branch >> 26;
            const uint32_t bo = (branch >> 21) & 0x1fu;
            const uint32_t bi = (branch >> 16) & 0x1fu;
            const uint32_t absolute = (branch >> 11) & 1u;
            const uint32_t link = branch & 1u;
            check::isTrue(opcode == 16u,
                          "the branch that skips is a conditional branch, opcode 16");
            check::isTrue(bo == 4u, "with BO=4, which branches when the bit BI names is false");
            check::isTrue(bi == 2u,
                          "and BI=2, the bit `andi.` sets when its result is not zero -- which is "
                          "what makes this bne and not the branch over the same displacement that "
                          "BI=0 gives");
            check::isTrue(absolute == 0u && link == 0u,
                          "and neither its absolute bit nor its link bit is set, so the "
                          "displacement is relative and the link register is the caller's");
            check::isTrue(
                branch == (0x40820000u | ((through - (kBlock + 4 * 6)) & 0xfffc)),
                "and the word is the title's own bne with the displacement to the through "
                "path, which is 0x4082....");
        }
        // A skipped call returns with the link register as the caller left it:
        // the gate has not touched r0 or r1 on this path.
        check::isTrue(words[7] == 0x4e800020, "and a skipped call returns: blr");
        // The through path begins *after* that return, and the ticks counter is
        // in it. It used to be on the skipped path, so the count the report calls
        // ticks counted the calls that did not run -- and a gate letting every
        // call through reported zero.
        check::isTrue(LogicGate::kThroughWord == 8,
                      "the through path starts after the skipped call's return, so a skipped "
                      "call cannot fall into the counter");
        check::isTrue(words[8] == (0x3c000000u | (5u << 21) | ((ticks >> 16) & 0xffff)),
                      "the path that runs the tick counts it: lis r5");
        check::isTrue(words[9] == (0x60000000u | (5u << 21) | (5u << 16) | (ticks & 0xffff)),
                      "and loads its low half into r5: ori r5,r5");
        check::isTrue(words[10] == (0x80000000u | (6u << 21) | (5u << 16)),
                      "reads it: lwz r6,0(r5)");
        check::isTrue(words[11] == (0x38000000u | (6u << 21) | (6u << 16) | 1u),
                      "adds one: addi r6,r6,1");
        check::isTrue(words[12] == (0x90000000u | (6u << 21) | (5u << 16)),
                      "stores it: stw r6,0(r5)");
        // The through path ends in a branch to the title's own *second* instruction.
        // The probe's stub has already run the first one, so the title runs the
        // rest itself. Branching to the word after the second is what froze the
        // title: the tick's epilogue returns through the link register its second
        // instruction saved into the caller's frame, so a tick that skipped the
        // store returns through a register nobody saved.
        check::isTrue(words[13] ==
                          ((18u << 26) | ((LogicGate::kTickBody - branchAt) & 0x03fffffcu)),
                      "and a call it lets through continues at the tick's own second instruction, "
                      "0x025d42f0, with the title running every instruction after it");
        // The displacement is measured from the word the branch stands in, which
        // is not where the through path starts. An earlier layout had the two as
        // one constant; with the through path six words long they are two, and
        // measuring from the start of the path lands 20 bytes past the tick's
        // second instruction -- which hangs the title and reads as a title that
        // has stopped. The expectation here is built from the branch's own index
        // rather than from `through`, because an expectation that reuses the
        // code's own expression cannot catch the code being wrong about it.
        check::isTrue(LogicGate::kBranchWord == 13, "the branch stands at word thirteen");
        check::isTrue(LogicGate::kThroughWord < LogicGate::kBranchWord,
                      "and the through path starts before it, so the path is what the branch ends");
        const uint32_t landed = branchAt + ((words[13] & 0x03fffffcu));
        check::isTrue(landed == LogicGate::kTickBody,
                      "read back the way the guest would, the branch lands on the tick's own "
                      "second instruction and nowhere else");
    }
    // The pass-through control has to reach the same instruction the gate's own
    // through path does. The indirect flavour used to load the *block's* address,
    // which branches the payload to itself: a loop with no exit, measured as the
    // display painting 0 times in an 8-second window.
    {
        const auto direct = LogicGate::throughPayload(kBlock, 1);
        check::isTrue(direct.size() == 1, "the direct pass-through is one word");
        check::isTrue(direct.size() == 1 &&
                          direct[0] ==
                              ((18u << 26) | ((LogicGate::kTickBody - kBlock) & 0x03fffffcu)),
                      "and it branches to the tick's own second instruction");
        for (int flavour = 1; flavour <= 2; flavour++) {
            const auto words = LogicGate::throughPayload(kBlock, flavour);
            check::isTrue(!words.empty(), "the pass-through control is built");
            if (words.empty()) {
                continue;
            }
            // Where each form actually branches, read back out of its own words.
            // The direct form is a primary branch: the target is the block plus
            // its displacement. The indirect form loads the address in two
            // instructions, so the target is the half in each of them.
            const uint32_t target = (flavour == 1)
                                        ? kBlock + (words[0] & 0x03fffffc)
                                        : ((words[0] & 0xffffu) << 16) | (words[1] & 0xffffu);
            check::isTrue(target != kBlock,
                          "no pass-through flavour branches to the block it lives in, which "
                          "would be a loop with no exit");
            check::isTrue(target == LogicGate::kTickBody,
                          "and every flavour reaches the tick's own second instruction, the "
                          "same place the direct form does");
        }
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
        // Two registrations on the tick: a momentary one that only asks for the
        // link-time moment and gives the entry back, and a standing one that
        // takes it and sends the call to the gate. The momentary one must not
        // keep the entry, or the standing one is refused for the rest of the run.
        check::isTrue(g_registrations.size() >= 2,
                      "the gate registers twice on the tick: once for the moment, once to "
                      "hold the entry and redirect");
        check::isTrue(!holdsEntry(LogicGate::kTick),
                      "and the momentary one does not keep the entry, so the standing one can "
                      "take it");
        check::isTrue(g_resume == kBlock,
                      "the standing probe on the tick resumes at the gate's own block, which is "
                      "the only way into the trampoline area known to arrive: a branch written "
                      "by the host into a guest function's interior does not");
        uint32_t word = 0;
        // The call arrives at that block whatever the gate is doing, so the block
        // holds a pass-through from the moment it exists. A freshly allocated
        // block is zeroes and a zero word is an illegal instruction, which is
        // what an un-gated gate did: not run the tick, but fault inside the
        // simulation.
        check::isTrue(readWord(kBlock, word) &&
                          word == ((18u << 26) | ((LogicGate::kTickBody - kBlock) & 0x03fffffcu)),
                      "and the gate's block is a bare pass-through to the tick's own body before "
                      "the gate is ever enabled");
        const uint32_t passThrough = kPassThroughAt;
        check::isTrue(gate.enable().empty(), "the gate installs");
        check::isTrue(readWord(kBlock, word) && word != passThrough,
                      "and enabling it puts the counting payload there instead of the "
                      "pass-through");
        // Enabling the *same* payload again changes nothing; enabling a different
        // one does. Without that, a run that installs the pass-through control
        // first cannot reach the gate at all -- the second enable answers "already
        // on" -- and the report says the gate is in while the block holds the
        // control, which is a measurement of the control called a gate.
        const std::vector<uint32_t> counting = LogicGate::payload(kBlock, kCounters);
        check::isTrue(gate.enable().empty(), "enabling it again is not a refusal");
        check::isTrue(readWord(kBlock, word) && word == counting[0],
                      "and leaves the same payload in place");
        check::isTrue(gate.enable(false, 2).empty(), "and asking for the same gate again is not "
                                                     "either");
        check::isTrue(gate.enable(true, 1).empty(), "but the pass-through control can be installed "
                                                    "over it");
        check::isTrue(readWord(kBlock, word) &&
                          word == ((18u << 26) | ((LogicGate::kTickBody - kBlock) & 0x03fffffcu)),
                      "and the block is that control's one word");
        check::isTrue(gate.enable(false, 2).empty(),
                      "and the gate can be reached again from the control without going out "
                      "first");
        check::isTrue(readWord(kBlock, word) && word == counting[0],
                      "with its own payload back in the block");
        // Nothing is written into the title's code. That is the change: a host
        // branch at an interior address does not arrive, and writing one would
        // also take the tick's second instruction away for good.
        check::isTrue(readWord(LogicGate::kTickBody, word) && word == LogicGate::kTickSecond,
                      "and the tick's own second instruction is left exactly as it was");
        check::isTrue(gate.resumeTarget() == kBlock, "and the probe resumes at the gate");
        check::isTrue(gate.enabled(), "the report says it is in");
        check::isTrue(gate.disable().empty(), "and it comes back out");
        // The stub's branch is fixed at install, so the call cannot be sent
        // somewhere else afterwards: the resume stays the gate's block and the
        // block goes back to its pass-through word. A freshly allocated block is
        // zeroes, and a zero word is an illegal instruction.
        check::isTrue(gate.resumeTarget() == kBlock,
                      "and out of the gate the call still arrives at the same block");
        check::isTrue(readWord(kBlock, word) &&
                          word == ((18u << 26) | ((LogicGate::kTickBody - kBlock) & 0x03fffffcu)),
                      "and the block is its pass-through word again, so a call that arrives now "
                      "runs the tick rather than whatever the payload left behind");
        check::isTrue(readWord(LogicGate::kTickBody, word) && word == LogicGate::kTickSecond,
                      "with the tick's own second instruction still where it was");
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
