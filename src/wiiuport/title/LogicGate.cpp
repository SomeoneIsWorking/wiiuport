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
constexpr uint32_t kLoadUpper = 0x3c000000;    // lis rD, hi
constexpr uint32_t kOrImmediate = 0x60000000;  // ori rS, rS, lo
constexpr uint32_t kLoadWord = 0x80000000;     // lwz rD, disp(rA)
constexpr uint32_t kAddImmediate = 0x38000000; // addi rD, rA, simm
constexpr uint32_t kStoreWord = 0x90000000;    // stw rS, disp(rA)
constexpr uint32_t kAndImmediate = 0x70000000; // andi. rS, rA, K
// `bne` is 0x40820000, and it was 0x40800000 here while the comment above named
// 0x40820014 as the instruction it came from. A conditional branch is one opcode
// with its condition in BO (bits 6-10) and BI (bits 11-15): BO=4 branches when the
// bit BI names is *false*, and `andi.` sets bit 2 (which is BI=2) when its result
// is not zero. So `bne` is BO=4 with BI=2, and BO=4 with BI=0 is a different
// branch over the same displacement. Four `bne` in this title's own image agree,
// and were read out of it rather than argued:
//
//   0x025d4398  bne 0x025d43ac  0x40820014
//   0x025d4678  bne 0x025d4740  0x408200c8
//   0x025d46a4  bne 0x025d4708  0x40820064
//   0x0274c964  bne 0x0274c938  0x4082ffd4
//
// and no instruction in the image carries 0x40800000 as a `bne`.
//
// What the wrong constant did is not subtle and not visible in any counter: with
// BO=4 BI=0 the branch is taken on an *even* count, so the gate let through the
// even calls and returned on the odd ones without running the tick -- and the
// title hung, reading exactly like a title that has stopped.
constexpr uint32_t kBranchNotEqual = 0x40820000; // bne, relative
constexpr uint32_t kCompareImmediate = 0x2c000000; // cmpwi rA, simm (0x025b020c)
constexpr uint32_t kReturn = 0x4e800020;         // blr
constexpr uint32_t kMoveToCounter = 0x7c0903a6;  // mtctr rS
constexpr uint32_t kBranchCount = 0x4e800420;    // bctr
// The skipped draw's frame, each word thousands of times in the image.
constexpr uint32_t kOpenFrame = 0x9421fff0;       // stwu r1,-0x10(r1)
constexpr uint32_t kSaveLink = 0x90010014;        // stw  r0,0x14(r1)
constexpr uint32_t kLoadLink = 0x80010014;        // lwz  r0,0x14(r1)
constexpr uint32_t kMoveToLink = 0x7c0803a6;      // mtlr r0
constexpr uint32_t kCloseFrame = 0x38210010;      // addi r1,r1,0x10
constexpr uint32_t kBranchCountLink = 0x4e800421; // bctrl
constexpr uint32_t kPrimaryBranch = 18;
constexpr int64_t kRelativeBranchReach = 0x02000000;

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
// The standing probe on the tick's entry. It counts every call -- so the host
// knows the call rate whether or not the gate is in -- and its resume is the
// gate's own block, which is how the gate is reached at all.
//
// Reached by the probe's stub branch, and that matters: a branch written by the
// host into a guest function's *interior* does not arrive in the trampoline area,
// whatever kind of branch it is, because the recompiler has to turn it into a jump
// to a host address it never translated. The stub's branch is the one way in that
// is known to work, and the caller census has counted 2020 calls a run through it.
class LogicGate::Counter final : public GuestCallProbes::Probe {
  public:
    explicit Counter(LogicGate& owner) : m_owner(owner) {
    }

    void OnInstall(GuestCallProbes::Installation installation) override;
    void OnCall(std::span<const uint32_t, 32>, uint32_t) override;

    // Atomic because the count is written from whichever guest thread called the
    // tick and read from the control channel's. A plain counter read across those
    // two is a race, and a racy count is a number nobody may report.
    std::atomic<uint64_t> calls{0};

  private:
    LogicGate& m_owner;
};

void LogicGate::Counter::OnInstall(GuestCallProbes::Installation installation) {
    m_owner.onCounted(installation);
}

void LogicGate::Counter::OnCall(std::span<const uint32_t, 32>, uint32_t) {
    calls.fetch_add(1, std::memory_order_relaxed);
}

class LogicGate::SceneWork final : public GuestCallProbes::Probe {
  public:
    explicit SceneWork(LogicGate& owner) : m_owner(owner) {
    }

    void OnInstall(GuestCallProbes::Installation installation) override {
        std::scoped_lock lock(m_owner.m_mutex);
        m_owner.m_sceneProbe = installation;
    }

    void OnCall(std::span<const uint32_t, 32>, uint32_t) override {
        calls.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<uint64_t> calls{0};

  private:
    LogicGate& m_owner;
};

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

LogicGate::LogicGate(Seams seams)
    : m_register(seams.registerProbe), m_allocateCode(seams.allocateCode),
      m_allocateData(seams.allocateData), m_writeWord(seams.writeWord), m_readWord(seams.readWord),
      m_moment(new Moment(*this)), m_counter(new Counter(*this)),
      m_sceneWork(new SceneWork(*this)) {
}

void LogicGate::install() {
    // Momentary, and the reason matters: the gate needs the link-time moment to
    // take its memory out of the loader's arena, and nothing after it. A probe
    // that kept the tick's entry would refuse every other probe on that address
    // for the rest of the run, so the caller census -- the only thing that counts
    // the simulation -- would see no calls at all. That is not a hypothetical: it
    // is what this registration did, and it read as a title that never ticks.
    m_register(kTick, kTickFirst, *m_moment, false, 0);
}

void LogicGate::onInstalled(GuestCallProbes::Installation installation) {
    std::scoped_lock lock(m_mutex);
    m_probe = installation;
    if (m_block != 0 || m_enabled) {
        return;
    }
    // Taken at link time, for the same reason the paint mod's block is: the
    // loader's arena is not something to ask while the title is running.
    m_block = m_allocateCode(kBlockBytes);
    if (m_block == 0) {
        m_refusal = "the loader's arena had no room for a logic gate";
        return;
    }
    // Writable memory for the two counters, from a different allocator because it
    // is a different kind of memory. See the header on why the code arena is the
    // wrong place for anything the guest has to store into.
    m_counters = m_allocateData(kCounterBytes);
    if (m_counters == 0) {
        m_refusal = "there was no writable guest memory for the gate's two counters";
        return;
    }
    // The block is filled before anything can reach it, because the standing
    // probe below sends *every* call here, gate or no gate. A freshly allocated
    // block is zeroes, and a zero word is an illegal instruction: an un-gated
    // gate did not run the title's tick, it raised a guest exception inside the
    // simulation, and the product died in the scheduler with a stack trace that
    // named nothing of this. "Out" is therefore a payload of its own -- one
    // branch, straight to the instruction after the entry -- and not the absence
    // of one.
    if (!passThrough()) {
        m_refusal = "the gate's block at " + hex(m_block) + " would not take its pass-through word";
        return;
    }
    // Now that the block exists, the standing probe: it takes the entry this
    // momentary one is about to give back, and it sends the call to the gate.
    // Registering from inside this callback is why the installer had to be fixed
    // to survive an append while it iterates.
    m_register(kTick, kTickFirst, *m_counter, true, m_block);
    // The scene gate reads only the skipping word, which is zero until a skipped call runs.
    std::vector<uint32_t> scene = scenePayload({.code = m_block, .counters = m_counters});
    if (scene.size() != kSceneGateWords) {
        m_refusal = "the scene gate at " + hex(m_block) + " cannot reach the scene's draw";
        return;
    }
    for (size_t word = 0; word < scene.size(); word++) {
        if (!m_writeWord(m_block + 4 * static_cast<uint32_t>(kSceneGateWord + word), scene[word])) {
            m_refusal = "the scene gate at " + hex(m_block) + " would not take word " +
                        std::to_string(word);
            return;
        }
    }
    m_register(kSceneWork, kSceneWorkFirst, *m_sceneWork, true,
               m_block + 4 * static_cast<uint32_t>(kSceneGateWord));
}

bool LogicGate::passThrough() {
    // The stub has already run the entry's own first instruction, so the title's
    // second is where a call that is not being gated belongs -- kTickBody, and not
    // the word after it. The tick's epilogue returns through the link register
    // its second instruction saved into the caller's frame, so continuing one word
    // too far does not merely run different code: it returns through a register
    // nobody saved, and the title spins in a wait loop forever with a control
    // channel still answering. This is the same one-word payload the `through`
    // control uses, deliberately: the control that proves a bare pass-through
    // arrives and the resting state of the gate are the same code, so there is no
    // way to tell them apart by accident.
    return m_writeWord(m_block, branchTo(m_block, kTickBody, false));
}

void LogicGate::onCounted(GuestCallProbes::Installation installation) {
    std::scoped_lock lock(m_mutex);
    m_counted = installation;
}

uint32_t LogicGate::resumeTarget() const {
    std::scoped_lock lock(m_mutex);
    return m_block.load();
}

std::vector<uint32_t> LogicGate::payload(Memory memory) {
    const uint32_t blockAddress = memory.code;
    const uint32_t countersAddress = memory.counters;
    // The gate runs in its own block, because a 52-byte tick has no room for it,
    // and the probe's stub is what brings a call here.
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
    // entry, so the gate keeps to them. Nothing here touches r0, r1 or the link
    // register: a skipped call returns through a link register the stub's own
    // `mfspr r0,LR` never disturbed, and a call let through continues at the
    // title's own second instruction with its registers as the title left them.
    //
    // The counters are in the data block: memory the guest writes, not memory it
    // executes from.
    uint32_t calls = countersAddress + 4 * kCallsWord;
    uint32_t ticks = countersAddress + 4 * kTicksWord;
    uint32_t testAt = blockAddress + 4 * 6;
    uint32_t through = blockAddress + 4 * kThroughWord;
    // Where the branch to the tick *stands*, which is not where the through path
    // starts. A displacement is measured from the word it is in; measured from the
    // start of the path instead, it lands 20 bytes past the tick's second
    // instruction and hangs the title with the gate armed.
    uint32_t branchAt = blockAddress + 4 * kBranchWord;
    uint32_t skippedAt = blockAddress + 4 * 7;
    uint32_t skippedDraw = blockAddress + 4 * kSkippedDrawWord;
    if (!withinReach(testAt, through) || !withinReach(branchAt, kTickBody)) {
        return {};
    }
    auto loadAddress = [](uint32_t reg, uint32_t address) {
        return std::array<uint32_t, 2>{kLoadUpper | (reg << 21) | ((address >> 16) & 0xffff),
                                       kOrImmediate | (reg << 21) | (reg << 16) |
                                           (address & 0xffff)};
    };
    uint32_t skipping = countersAddress + 4 * kSkippingWord;
    const auto skippingAt = loadAddress(11, skipping);
    const auto matrixInit = loadAddress(12, kMatrixInit);
    const auto iterator = loadAddress(3, kDrawIterator);
    const auto process = loadAddress(4, kDrawProcess);
    const auto handler = loadAddress(12, kDrawHandler);
    // The ticks counter is incremented on the path that *runs* the tick, not on
    // the one that skips it. It read the other way round, which made the report
    // say "0 ticks through it" for a gate that was letting every call through --
    // a counter named for one thing and counting the other, and a report nobody
    // can check. With the ticks on the through path, the count that answers
    // "how many ticks ran" is the count the report calls ticks, and the count of
    // skipped calls is calls minus ticks.
    return {
        kLoadUpper | (3 << 21) | ((calls >> 16) & 0xffff),       // lis  r3, calls
        kOrImmediate | (3 << 21) | (3 << 16) | (calls & 0xffff), // ori  r3, r3, calls
        kLoadWord | (4 << 21) | (3 << 16),                       // lwz  r4, 0(r3)
        kAddImmediate | (4 << 21) | (4 << 16) | 1,               // addi r4, r4, 1
        kStoreWord | (4 << 21) | (3 << 16),                      // stw  r4, 0(r3)
        kAndImmediate | (4 << 21) | (4 << 16) | 1,               // andi. r4, r4, 1
        kBranchNotEqual | ((through - testAt) & 0xfffc),         // bne  the through path
        branchTo(skippedAt, skippedDraw, false),                 // b    the skipped draw
        kLoadUpper | (5 << 21) | ((ticks >> 16) & 0xffff),       // lis  r5, ticks
        kOrImmediate | (5 << 21) | (5 << 16) | (ticks & 0xffff), // ori  r5, r5, ticks
        kLoadWord | (6 << 21) | (5 << 16),                       // lwz  r6, 0(r5)
        kAddImmediate | (6 << 21) | (6 << 16) | 1,               // addi r6, r6, 1
        kStoreWord | (6 << 21) | (5 << 16),                      // stw  r6, 0(r5)
        branchTo(branchAt, kTickBody, false),                    // b    the tick's own body
        // r0 holds the caller's link register: the stub ran the tick's `mflr r0`.
        kOpenFrame,
        kSaveLink,
        skippingAt[0],
        skippingAt[1],
        kAddImmediate | (12 << 21) | 1,  // li   r12, 1
        kStoreWord | (12 << 21) | (11 << 16), // stw  r12, 0(r11)
        matrixInit[0],
        matrixInit[1],
        kMoveToCounter | (12 << 21),
        kBranchCountLink,
        iterator[0],
        iterator[1],
        process[0],
        process[1],
        handler[0],
        handler[1],
        kMoveToCounter | (12 << 21),
        kBranchCountLink,
        skippingAt[0],
        skippingAt[1],
        kAddImmediate | (12 << 21),           // li   r12, 0
        kStoreWord | (12 << 21) | (11 << 16), // stw  r12, 0(r11)
        kLoadLink,
        kMoveToLink,
        kCloseFrame,
        kReturn,
    };
}

std::vector<uint32_t> LogicGate::scenePayload(Memory memory) {
    uint32_t skipping = memory.counters + 4 * kSkippingWord;
    uint32_t testAt = memory.code + 4 * (kSceneGateWord + 4);
    uint32_t onAt = memory.code + 4 * (kSceneGateWord + 5);
    uint32_t loopAt = memory.code + 4 * (kSceneGateWord + 6);
    if (!withinReach(onAt, kSceneWork + 4) || !withinReach(loopAt, kSceneDrawLoop)) {
        return {};
    }
    // r12 carried the stub's branch here, and r0, r11, r12 and cr0 are dead at kSceneWork.
    return {
        kLoadUpper | (12 << 21) | ((skipping >> 16) & 0xffff),          // lis   r12, skipping
        kOrImmediate | (12 << 21) | (12 << 16) | (skipping & 0xffff),   // ori   r12, r12, skipping
        kLoadWord | (12 << 21) | (12 << 16),                            // lwz   r12, 0(r12)
        kCompareImmediate | (12 << 16),                                 // cmpwi r12, 0
        kBranchNotEqual | ((loopAt - testAt) & 0xfffc),                 // bne   the draw loop
        branchTo(onAt, kSceneWork + 4, false),                          // b     the tick's work
        branchTo(loopAt, kSceneDrawLoop, false),                        // b     the draw loop
    };
}

std::vector<uint32_t> LogicGate::throughPayload(uint32_t blockAddress, Through flavour) {
    // One word that branches to the title's own second instruction: no state,
    // nothing to keep right, and the tick either runs or does not. The census says
    // which, and it counts the tick whether or not this payload runs.
    if (flavour == Through::Direct) {
        return {branchTo(blockAddress, kTickBody, false)};
    }
    // Through the count register, which is the way the recompiler resolves a
    // target it has a jump-table entry for -- the way a stand-in reached through a
    // vtable runs. `mtctr` does not touch the link register, so the tick's return
    // still reaches the title's caller.
    //
    // The address loaded is the one the direct form branches to, the tick's own
    // second instruction. Loading the *block's* address here instead -- which is
    // what this did -- branches to the payload itself, and a payload that
    // branches to itself is a loop with no exit: measured as the display painting
    // 0 times in an 8-second window with the control channel still answering,
    // which reads exactly like a title that has stopped and is not one.
    return {
        kLoadUpper | (12 << 21) | ((kTickBody >> 16) & 0xffff),        // lis  r12,hi
        kOrImmediate | (12 << 21) | (12 << 16) | (kTickBody & 0xffff), // ori  r12,r12,lo
        kMoveToCounter | (12 << 21),                                   // mtctr r12
        kBranchCount,                                                  // bctr
    };
}

std::string LogicGate::enable(bool through, int throughFlavour) {
    std::scoped_lock lock(m_mutex);
    m_throughFlavour = throughFlavour;
    // Not an early return when it is already in. Enabling is a request for a
    // *particular* payload, and a gate that answers "already on" without looking
    // at which one is in cannot be moved from the pass-through control to the
    // counting one without going out first -- which is how a run ends up
    // measuring a control and calling it a gate, or the other way round, with the
    // report saying only that the gate is on.
    if (m_enabled && m_through == through && m_installedFlavour == throughFlavour) {
        return {};
    }
    if (m_block == 0 || m_counters == 0) {
        // Named by which half is missing, because "no guest memory" for a gate
        // whose instructions are allocated and whose counters are not is a
        // sentence that sends a reader to the wrong allocator.
        if (m_block == 0) {
            return m_refusal.empty() ? "no guest memory was reserved for a logic gate" : m_refusal;
        }
        return m_refusal.empty() ? "no writable guest memory was reserved for the gate's counters"
                                 : m_refusal;
    }
    // The word the gate takes is the tick's second instruction, and it is
    // checked rather than assumed: a revision whose tick starts differently gets a
    // refusal instead of a gate that ran the wrong code. The gate does not
    // replace it -- nothing is written into the title's code at all -- but the
    // gate's through path continues at that instruction, so its identity is part
    // of what the gate is for.
    uint32_t second = 0;
    if (!m_readWord(kTickBody, second) || second != kTickSecond) {
        m_refusal = "the tick's body at " + hex(kTickBody) + " holds " + hex(second) + ", not " +
                    hex(kTickSecond) + "; this gate is written for this title's tick";
        return m_refusal;
    }
    // `through` is the control, and which way it branches *is* the experiment:
    // 1 is a direct branch and 2 an indirect one through the count register, which
    // is the mechanism the recompiler's jump table serves. Everything else about
    // the install is identical, and the observer is the caller census.
    const std::vector<uint32_t> control =
        through ? throughPayload(m_block, static_cast<Through>(m_throughFlavour))
                : std::vector<uint32_t>{};
    const std::vector<uint32_t> words =
        through ? control : payload({.code = m_block, .counters = m_counters});
    if (through ? (m_throughFlavour < 1 || words.empty()) : (words.size() != kGateWords)) {
        m_refusal = "the gate at " + hex(m_block) + " cannot reach the tick's body";
        return m_refusal;
    }
    for (size_t word = 0; word < words.size(); word++) {
        if (!m_writeWord(m_block + 4 * static_cast<uint32_t>(word), words[word])) {
            m_refusal = "the gate's block at " + hex(m_block) + " would not take word " +
                        std::to_string(word);
            return m_refusal;
        }
    }
    // Nothing is written into the title's code. The probe's own stub sends the
    // call here, and the words in the tick's body are left exactly as they were:
    // a host-written branch at an interior address does not arrive, and writing
    // one there would also take the tick's second instruction away for good.
    m_enabled = true;
    m_through = through;
    m_installedFlavour = throughFlavour;
    m_refusal.clear();
    lucent::info("gate", "logic gate at {}: the tick at {} now runs every other call{}",
                 hex(m_block), hex(kTick),
                 through ? (m_throughFlavour == 1
                                ? ", as a bare pass-through reached by a direct branch"
                                : ", as a bare pass-through reached through the count register")
                         : "");
    return {};
}

std::string LogicGate::disable() {
    std::scoped_lock lock(m_mutex);
    if (!m_enabled) {
        return {};
    }
    // The gate's own block goes back to its pass-through word. The title's code
    // is untouched and always was: the probe holds the entry, and the block is
    // where the decision lives.
    if (!passThrough()) {
        m_refusal = "the gate's block at " + hex(m_block) + " would not take its pass-through word";
        return m_refusal;
    }
    m_enabled = false;
    m_through = false;
    m_installedFlavour = 0;
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
    // The counters' address, not the code block's: they are two different kinds
    // of memory now, and a report that named the code block for both would send a
    // reader to look for a count in sixteen instructions.
    body.string("counters", hex(m_counters));
    body.string("calls", hex(m_counters + 4 * kCallsWord));
    body.string("ticks", hex(m_counters + 4 * kTicksWord));
    body.number("blockBytes", kBlockBytes);
    body.number("counterBytes", kCounterBytes);
    // What the fork says about the probe that holds the tick's entry, and what
    // the entry's next word actually holds right now. Between them they say
    // whether a zero count means "no calls came" or "the gate is not wired to
    // the tick", which are the same number and not the same finding.
    body.string("probe", std::string(installationName(m_probe)));
    // The standing probe that actually holds the entry, and the calls it counted.
    // These are the two numbers that say whether the gate is reached at all, and
    // they are separate from the counters above because they are kept by the host
    // rather than by the guest: the guest's own counter cannot tell "the gate did
    // not run" from "the gate ran and its store did not land", and those are the
    // two things worth telling apart.
    body.string("holdingProbe", std::string(installationName(m_counted)));
    body.number("callsAtProbe", m_counter != nullptr ? m_counter->calls.load() : 0);
    body.string("sceneProbe", std::string(installationName(m_sceneProbe)));
    body.number("sceneCalls", m_sceneWork != nullptr ? m_sceneWork->calls.load() : 0);
    // Which payload the block holds, because "enabled" alone cannot say it: the
    // pass-through control is enabled too, and it is the control, not the gate.
    body.raw("through", m_through ? "true" : "false");
    body.number("flavour", m_through ? m_installedFlavour : 0);
    // Read straight from the member, not through resumeTarget(): the report
    // already holds the lock, and taking it again in the same call is a
    // self-deadlock that hangs the first test to ask the gate anything. The
    // accessor is for callers that do not hold it.
    body.string("resume", hex(m_block.load()));
    // Quoted, because a client parses this: `raw` writes a value verbatim, and a
    // bare `0x90010004` is not JSON. That is not a cosmetic slip -- the route's
    // only consumer is a JSON parser, so one unquoted word made the whole report
    // unreadable and every caller fell back to a probe the gate holds, which
    // reads as a simulation running at zero hertz.
    uint32_t atBody = 0;
    if (m_readWord(kTickBody, atBody)) {
        body.string("wordAtTickBody", hex(atBody));
    } else {
        body.raw("wordAtTickBody", "null");
    }
    uint32_t atEntry = 0;
    if (m_readWord(kTick, atEntry)) {
        body.string("wordAtTickEntry", hex(atEntry));
    } else {
        body.raw("wordAtTickEntry", "null");
    }
    body.raw("enabled", m_enabled ? "true" : "false");
    uint32_t calls = 0;
    uint32_t ticks = 0;
    if (m_counters != 0 && m_readWord(m_counters + 4 * kCallsWord, calls)) {
        body.number("callsCount", calls);
    } else {
        body.raw("callsCount", "null");
    }
    if (m_counters != 0 && m_readWord(m_counters + 4 * kTicksWord, ticks)) {
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
