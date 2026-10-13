#include "wiiuport/title/ProControllerChoice.h"

#include "wiiuport/guest/PowerPcWords.h"
#include "wiiuport/guest/ProbeInstallation.h"

#include <lucent/log.h>

#include <array>
#include <cstdio>

namespace wiiuport::title {

namespace {

// Lifted from the Display exec itself.
constexpr uint32_t kSaveRegisters = 0xbe810058;     // stmw r20,0x58(r1)   0x026d4410
constexpr uint32_t kReadLink = 0x7c0802a6;          // mflr r0             0x026d4414
constexpr uint32_t kSaveDisplayLink = 0x9001008c;   // stw  r0,0x8c(r1)    0x026d4418
constexpr uint32_t kKeepScreen = 0x7c7c1b78;        // mr   r28,r3         0x026d4428
constexpr uint32_t kCompareResult = 0x2c030000;     // cmpwi r3,0          0x026d45e0
constexpr uint32_t kChooseProButton = 0x931c0054;   // stw  r24,0x54(r28)  0x026d45d8
constexpr uint32_t kStateMachine = 0x387c0018;      // addi r3,r28,0x18    0x026d45e8
constexpr uint32_t kRestoreRegisters = 0xba810058;  // lmw  r20,0x58(r1)   0x026d45f4
constexpr uint32_t kLoadDisplayLink = 0x8001008c;   // lwz  r0,0x8c(r1)    0x026d45f8
constexpr uint32_t kCloseDisplayFrame = 0x38210088; // addi r1,r1,0x88     0x026d4600
constexpr uint32_t kProButton = 1;
constexpr uint32_t kButtonRegister = 24;
// mr r3,r28: the field layout of `mr r28,r3` with the registers swapped.
constexpr uint32_t kRestoreScreen = 0x7f83e378;
constexpr size_t kNotReadyWord = 24;
constexpr size_t kReadyBranchWord = 9;
constexpr size_t kBodyBranchWord = 28;

std::string hex(uint32_t value) {
    std::array<char, 11> text{};
    std::snprintf(text.data(), text.size(), "0x%08x", value);
    return {text.data()};
}

} // namespace

class ProControllerChoice::Moment final : public GuestCallProbes::Probe {
  public:
    explicit Moment(ProControllerChoice& owner) : m_owner(owner) {
    }

    void OnInstall(GuestCallProbes::Installation) override {
        m_owner.onLinked();
    }

    void OnCall(std::span<const uint32_t, 32>, uint32_t) override {
    }

  private:
    ProControllerChoice& m_owner;
};

class ProControllerChoice::Display final : public GuestCallProbes::Probe {
  public:
    explicit Display(ProControllerChoice& owner) : m_owner(owner) {
    }

    void OnInstall(GuestCallProbes::Installation installation) override {
        std::scoped_lock lock(m_owner.m_mutex);
        m_owner.m_installation = installation;
    }

    void OnCall(std::span<const uint32_t, 32>, uint32_t) override {
        frames.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<uint64_t> frames{0};

  private:
    ProControllerChoice& m_owner;
};

std::vector<uint32_t> ProControllerChoice::payload(uint32_t block) {
    using namespace guest::ppc;
    const uint32_t bodyBranchAt = block + 4 * static_cast<uint32_t>(kBodyBranchWord);
    if (!withinReach(bodyBranchAt, kDisplayBody)) {
        return {};
    }
    const auto ready = loadAddress(12, kProControllerReady);
    const auto outAll = loadAddress(4, kOutAll);
    const auto changeState = loadAddress(12, kChangeState);
    const uint32_t toNotReady = 4 * static_cast<uint32_t>(kNotReadyWord - kReadyBranchWord);
    // Entered from the probe's stub, which ran the entry's `stwu`: the frame is open, r3 is
    // the screen and the link register is the caller's.
    return {
        kSaveRegisters,
        kReadLink,
        kSaveDisplayLink,
        kKeepScreen,
        ready[0],
        ready[1],
        kMoveToCounter | (12 << 21),
        kBranchCountLink,
        kCompareResult,
        kBranchEqual | toNotReady,
        kAddImmediate | (kButtonRegister << 21) | kProButton, // li r24,1
        kChooseProButton,
        kStateMachine,
        outAll[0],
        outAll[1],
        changeState[0],
        changeState[1],
        kMoveToCounter | (12 << 21),
        kBranchCountLink,
        kRestoreRegisters,
        kLoadDisplayLink,
        kMoveToLink,
        kCloseDisplayFrame,
        kReturn,
        // No Pro Controller: the frame continues at the exec's second instruction as entered.
        kRestoreScreen,
        kLoadDisplayLink,
        kMoveToLink,
        kRestoreRegisters,
        branchTo(bodyBranchAt, kDisplayBody, false),
    };
}

ProControllerChoice::ProControllerChoice(Seams seams)
    : m_seams(seams), m_moment(new Moment(*this)), m_display(new Display(*this)) {
}

ProControllerChoice::~ProControllerChoice() = default;

void ProControllerChoice::install() {
    m_seams.registerProbe(kDisplay, kDisplayFirst, *m_moment, false, 0);
}

void ProControllerChoice::onLinked() {
    std::scoped_lock lock(m_mutex);
    if (m_block != 0) {
        return;
    }
    m_block = m_seams.allocateCode(kBlockBytes);
    if (m_block == 0) {
        m_refusal = "the loader's arena had no room for the controller choice";
        lucent::error("controller", "{}", m_refusal);
        return;
    }
    std::vector<uint32_t> words = payload(m_block);
    if (words.size() != kBlockWords) {
        m_refusal = "the block at " + hex(m_block) + " cannot reach the Display exec";
        lucent::error("controller", "{}", m_refusal);
        return;
    }
    for (size_t word = 0; word < words.size(); word++) {
        if (!m_seams.writeWord(m_block + 4 * static_cast<uint32_t>(word), words[word])) {
            m_refusal =
                "the block at " + hex(m_block) + " would not take word " + std::to_string(word);
            lucent::error("controller", "{}", m_refusal);
            return;
        }
    }
    // The block is complete before the probe can send a call to it.
    m_seams.registerProbe(kDisplay, kDisplayFirst, *m_display, true, m_block);
    lucent::info("controller", "Controller Selection takes the Pro Controller, block at {}",
                 hex(m_block));
}

uint64_t ProControllerChoice::displayFrames() const {
    return m_display->frames.load(std::memory_order_relaxed);
}

std::string ProControllerChoice::json() const {
    std::scoped_lock lock(m_mutex);
    return std::string("{\"probe\":\"") + std::string(guest::installationName(m_installation)) +
           "\",\"block\":\"" + hex(m_block) +
           "\",\"displayFrames\":" + std::to_string(displayFrames()) + ",\"refusal\":\"" +
           m_refusal + "\"}\n";
}

} // namespace wiiuport::title
