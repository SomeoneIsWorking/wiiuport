#include "wiiuport/title/LogicGate.h"

#include <lucent/log.h>

#include <array>
#include <cstdio>
#include <vector>

namespace wiiuport::title {

namespace {

// Every word the gate needs, lifted from the title's own image. The three that
// carry a register are derived from two instructions each that differ only in
// that field, which is what makes the field a fact about this title's encoding
// rather than a guess:
//
//   `lis r0,0x1000` 0x3c001000 and `lis r12,0x1019` 0x3d801019 -> rS at 21-25
//   `ori r0,r0,0x7431` 0x60007431                               -> rS, rD at 21-25
//   `lwz r12,0x24(r31)` 0x819f0024 and `lwz r0,0xcc(r12)` 0x800c00cc
//   `addi` from `li r3,1` 0x38600001
//   `stw r31,0xc(r1)` 0x93e1000c and `stw r0,0x14(r1)` 0x981f0014
//   `andi. r0,r7,0xfd` 0x70e000fd
//   `bne 0x0200004c` 0x40820014, without its absolute bit 0x40820000
//   `blr` 0x4e800020 from 0x025f0950
constexpr uint32_t kLoadUpper = 0x3c000000;      // lis rD, hi
constexpr uint32_t kOrImmediate = 0x60000000;    // ori rS, rS, lo
constexpr uint32_t kLoadWord = 0x80000000;       // lwz rD, disp(rA)
constexpr uint32_t kAddImmediate = 0x38000000;   // addi rD, rA, simm
constexpr uint32_t kStoreWord = 0x90000000;      // stw rS, disp(rA)
constexpr uint32_t kAndImmediate = 0x70000000;   // andi. rS, rA, K
constexpr uint32_t kBranchNotEqual = 0x40800000; // bne, relative
constexpr uint32_t kReturn = 0x4e800020;         // blr
constexpr uint32_t kPrimaryBranch = 18;
constexpr int64_t kRelativeBranchReach = 0x02000000;

// The tick's own first two instructions, lifted whole out of the title's image
// at 0x025d42ec and 0x025d42f0 -- bytes read from it, not assembled here:
//
//     025d42ec  mfspr r0,LR        7c0802a6
//     025d42f0  stw  r0,0x4(r1)    90010004
//
// The gate has to supply them, because it takes the word at 0x025d42f0 for its
// own branch, and the tick cannot be entered past them: its epilogue reads the
// saved link register back out of the caller's frame at 0x4(r1) and returns
// through it, so a tick that skipped the store would return to whatever LR held
// when the gate ran. This is the whole reason the gate replaces the *second*
// instruction rather than branching around the first.
constexpr uint32_t kTickSaveLink = 0x7c0802a6;  // mfspr r0,LR
constexpr uint32_t kTickStoreLink = 0x90010004; // stw  r0,0x4(r1)

std::string hex(uint32_t value) {
    std::array<char, 11> text{};
    std::snprintf(text.data(), text.size(), "0x%08x", value);
    return {text.data()};
}

// A JSON object written one member at a time, owning the separators.
class JsonBody {
  public:
    void raw(const char* name, const std::string& value) {
        if (!m_first) {
            m_body += ',';
        }
        m_first = false;
        m_body += '"';
        m_body += name;
        m_body += "\":";
        m_body += value;
    }

    void string(const char* name, const std::string& value) {
        raw(name, "\"" + value + "\"");
    }

    void number(const char* name, uint64_t value) {
        raw(name, std::to_string(value));
    }

    std::string finish() const {
        return m_body + "}\n";
    }

  private:
    std::string m_body = "{";
    bool m_first = true;
};

// The fork's own names for a probe's installation, so the report says "the
// entry was held by another probe" in words rather than as an enumeration.
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

uint32_t branchTo(uint32_t from, uint32_t to, bool link) {
    return (kPrimaryBranch << 26) | (link ? 1u : 0u) | ((to - from) & 0x03fffffcu);
}

bool withinReach(uint32_t from, uint32_t to) {
    const int64_t displacement = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    return displacement >= -kRelativeBranchReach && displacement < kRelativeBranchReach;
}

} // namespace

// Only for the link-time moment: the gate replaces the tick's entry, so a probe
// there would be counting calls the gate has taken over.
class LogicGate::Moment final : public GuestCallProbes::Probe {
  public:
    explicit Moment(LogicGate& owner) : m_owner(owner) {
    }

    void OnInstall(GuestCallProbes::Installation installation) override;
    void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;

  private:
    LogicGate& m_owner;
};

void LogicGate::Moment::OnInstall(GuestCallProbes::Installation installation) {
    m_owner.onInstalled(installation);
}

void LogicGate::Moment::OnCall(std::span<const uint32_t, 32>, uint32_t) {
}

LogicGate::LogicGate(Register registerProbe, AllocateCode allocateCode, WriteWord writeWord,
                     ReadWord readWord)
    : m_register(registerProbe), m_allocateCode(allocateCode), m_writeWord(writeWord),
      m_readWord(readWord), m_moment(new Moment(*this)) {
}

void LogicGate::install() {
    m_register(kTick, kTickFirst, *m_moment);
}

void LogicGate::onInstalled(GuestCallProbes::Installation installation) {
    std::scoped_lock lock(m_mutex);
    m_probe = installation;
    if (m_block != 0 || m_enabled) {
        return;
    }
    // Taken at link time, for the same reason the paint mod's block is: the
    // loader's arena is not something to ask while the title is running.
    m_block = m_allocateCode(4 * kBlockWords);
    if (m_block == 0) {
        m_refusal = "the loader's arena had no room for a logic gate";
    }
}

std::vector<uint32_t> LogicGate::payload(uint32_t blockAddress) {
    // The gate runs in its own block, because a 52-byte tick has no room for it.
    // The word at 0x025d42f0 -- the tick's second instruction -- takes a branch
    // to here, and the gate's return and its tail branch both go to the title's
    // own code. It supplies the tick's first two instructions itself, so a call
    // it lets through is the title's tick entered the way the title enters it.
    //
    // Every word here is either lifted from the title's image or a branch, and
    // each lifted form was checked against a second instruction in that image
    // before being used. `ori` and `addi` both name their destination in bits
    // 16-20 and their source in bits 21-25, and a payload that puts the
    // destination in the source's field builds the address in r0 instead of r3 --
    // where the next word overwrites it before anything reads it. The forms used
    // here are the title's own: `ori r0,r0,0x7431` (0x60007431) and
    // `addi r1,r1,0x8` (0x38210008).
    //
    // r3 to r6 are volatile under the EABI and the tick reads none of them on
    // entry, so the gate keeps to them. r0 and r1 are untouched until the through
    // path, where the title's own two instructions touch them.
    const uint32_t calls = blockAddress + 4 * kCallsWord;
    const uint32_t ticks = blockAddress + 4 * kTicksWord;
    const uint32_t testAt = blockAddress + 4 * 6;
    const uint32_t through = blockAddress + 4 * kThroughWord;
    const uint32_t tailAt = through + 8;
    if (!withinReach(testAt, through) || !withinReach(tailAt, kTickBody + 4)) {
        return {};
    }
    return {
        kLoadUpper | (3 << 21) | ((calls >> 16) & 0xffff),       // lis  r3, calls
        kOrImmediate | (3 << 21) | (3 << 16) | (calls & 0xffff), // ori  r3, r3, calls
        kLoadWord | (4 << 21) | (3 << 16),                       // lwz  r4, 0(r3)
        kAddImmediate | (4 << 21) | (4 << 16) | 1,               // addi r4, r4, 1
        kStoreWord | (4 << 21) | (3 << 16),                      // stw  r4, 0(r3)
        kAndImmediate | (4 << 21) | (4 << 16) | 1,               // andi. r4, r4, 1
        kBranchNotEqual | ((through - testAt) & 0xfffc),         // bne  the through path
        kLoadUpper | (5 << 21) | ((ticks >> 16) & 0xffff),       // lis  r5, ticks
        kOrImmediate | (5 << 21) | (5 << 16) | (ticks & 0xffff), // ori  r5, r5, ticks
        kLoadWord | (6 << 21) | (5 << 16),                       // lwz  r6, 0(r5)
        kAddImmediate | (6 << 21) | (6 << 16) | 1,               // addi r6, r6, 1
        kStoreWord | (6 << 21) | (5 << 16),                      // stw  r6, 0(r5)
        kReturn,                                                 // blr  on a skipped call
        kTickSaveLink,                                           // mfspr r0,LR
        kTickStoreLink,                                          // stw  r0,0x4(r1)
        branchTo(tailAt, kTickBody + 4, false),                  // b    the tick's own body
    };
}

std::string LogicGate::enable() {
    std::scoped_lock lock(m_mutex);
    if (m_enabled) {
        return {};
    }
    if (m_block == 0) {
        return m_refusal.empty() ? "no guest memory was reserved for a logic gate" : m_refusal;
    }
    // The word the gate takes is the tick's second instruction, and it is
    // checked rather than assumed: a revision whose tick starts differently gets a
    // refusal instead of a gate that ran the wrong code. The first instruction is
    // not checked here because the gate does not replace it -- the probe has --
    // and the gate supplies its own copy.
    uint32_t first = 0;
    if (!m_readWord(kTickBody, first) || first != kTickSecond) {
        m_refusal = "the tick's body at " + hex(kTickBody) + " holds " + hex(first) + ", not " +
                    hex(kTickSecond) + "; this gate is written for this title's tick";
        return m_refusal;
    }
    m_original = first;
    const std::vector<uint32_t> words = payload(m_block);
    if (words.size() != kGateWords) {
        m_refusal = "the gate at " + hex(m_block) + " cannot reach the tick's body or its skip";
        return m_refusal;
    }
    for (size_t word = 0; word < words.size(); word++) {
        if (!m_writeWord(m_block + 4 * static_cast<uint32_t>(word), words[word])) {
            m_refusal = "the gate's block at " + hex(m_block) + " would not take word " +
                        std::to_string(word);
            return m_refusal;
        }
    }
    const uint32_t entry = branchTo(kTickBody, m_block, false);
    if (!withinReach(kTickBody, m_block)) {
        m_refusal =
            "the gate at " + hex(m_block) + " is out of a branch's reach of " + hex(kTickBody);
        return m_refusal;
    }
    if (!m_writeWord(kTickBody, entry)) {
        m_refusal = "the tick's body at " + hex(kTickBody) + " would not take the branch";
        return m_refusal;
    }
    m_enabled = true;
    m_refusal.clear();
    lucent::info("gate", "logic gate at {}: the tick at {} now runs every other call", hex(m_block),
                 hex(kTick));
    return {};
}

std::string LogicGate::disable() {
    std::scoped_lock lock(m_mutex);
    if (!m_enabled) {
        return {};
    }
    if (!m_writeWord(kTickBody, m_original)) {
        m_refusal =
            "the tick's body at " + hex(kTickBody) + " would not take " + hex(m_original) + " back";
        return m_refusal;
    }
    m_enabled = false;
    m_refusal.clear();
    lucent::info("gate", "the tick at {} is the title's own again", hex(kTick));
    return {};
}

std::string LogicGate::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("tick", hex(kTick));
    body.string("frameEntry", hex(kFrameEntry));
    body.string("block", hex(m_block));
    body.string("calls", hex(m_block + 4 * kCallsWord));
    body.string("ticks", hex(m_block + 4 * kTicksWord));
    body.number("blockBytes", kBlockBytes);
    // What the fork says about the probe that holds the tick's entry, and what
    // the entry's next word actually holds right now. Between them they say
    // whether a zero count means "no calls came" or "the gate is not wired to
    // the tick", which are the same number and not the same finding.
    body.string("probe", std::string(installationName(m_probe)));
    uint32_t atBody = 0;
    body.raw("wordAtTickBody", m_readWord(kTickBody, atBody) ? hex(atBody) : std::string("null"));
    uint32_t atEntry = 0;
    body.raw("wordAtTickEntry", m_readWord(kTick, atEntry) ? hex(atEntry) : std::string("null"));
    body.raw("enabled", m_enabled ? "true" : "false");
    uint32_t calls = 0;
    uint32_t ticks = 0;
    if (m_block != 0 && m_readWord(m_block + 4 * kCallsWord, calls)) {
        body.number("callsCount", calls);
    } else {
        body.raw("callsCount", "null");
    }
    if (m_block != 0 && m_readWord(m_block + 4 * kTicksWord, ticks)) {
        body.number("ticksCount", ticks);
    } else {
        body.raw("ticksCount", "null");
    }
    if (!m_refusal.empty()) {
        body.string("refusal", m_refusal);
    }
    return body.finish();
}

} // namespace wiiuport::title
