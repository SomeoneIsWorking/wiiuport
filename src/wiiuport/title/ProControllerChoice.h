#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD opens Controller Selection, GamePad or Pro Controller, every time its file select
// starts. The product's controller is a Pro Controller, so the screen makes that choice on its
// first Display frame through its own Pro path: the button index, the presence check
// `FUN_026d43dc`, and the change to StateID_OutAll, after which the scene confirms mode 0. With no
// Pro Controller on channel 0 the frame runs as the title wrote it.
class ProControllerChoice {
  public:
    // StateID_Display's exec on the Controller Selection screen (screen in r3).
    static constexpr uint32_t kDisplay = 0x026d440c;
    static constexpr uint32_t kDisplayFirst = 0x9421ff78; // stwu r1,-0x88(r1)
    static constexpr uint32_t kDisplayBody = kDisplay + 4;
    static constexpr uint32_t kProControllerReady = 0x026d43dc;
    // sead state machine changeState(machine, state).
    static constexpr uint32_t kChangeState = 0x020063c0;
    static constexpr uint32_t kOutAll = 0x1049aaa4;
    static constexpr size_t kBlockWords = 29;
    static constexpr uint32_t kBlockBytes = 4 * kBlockWords;

    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry, uint32_t resume);
    using AllocateCode = uint32_t (*)(uint32_t sizeInBytes);
    using WriteWord = bool (*)(uint32_t guestAddress, uint32_t value);

    struct Seams {
        Register registerProbe;
        AllocateCode allocateCode;
        WriteWord writeWord;
    };

    // The block's words at `block`; empty when the Display body is out of a branch's reach.
    static std::vector<uint32_t> payload(uint32_t block);

    explicit ProControllerChoice(Seams seams);
    ~ProControllerChoice();
    ProControllerChoice(const ProControllerChoice&) = delete;
    ProControllerChoice& operator=(const ProControllerChoice&) = delete;
    ProControllerChoice(ProControllerChoice&&) = delete;
    ProControllerChoice& operator=(ProControllerChoice&&) = delete;

    // Asks for the link-time moment, when the block is taken and the Display exec probed.
    void install();

    // Display frames that reached the block.
    uint64_t displayFrames() const;
    std::string json() const;

  private:
    class Moment;
    class Display;

    void onLinked();

    Seams m_seams;
    std::unique_ptr<Moment> m_moment;
    std::unique_ptr<Display> m_display;
    mutable std::mutex m_mutex;
    uint32_t m_block = 0;
    std::optional<GuestCallProbes::Installation> m_installation;
    std::string m_refusal;
};

} // namespace wiiuport::title
