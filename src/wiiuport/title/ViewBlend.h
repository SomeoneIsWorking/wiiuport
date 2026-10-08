#pragma once

#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/title/CommandStreamIdentity.h"

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace wiiuport::title {

// The camera's view, blended on the in-between paint.
//
// The model renderer uploads its view as uniform key 1: `uploadView(ctx)` sends the twelve words at
// `ctx+0x70` unless `ctx+0xb6` says the bound program has them. On the display thread the probe
// records which packet will carry the view and whether this is the in-between paint; on the Latte
// thread that packet reaches the register file, where the tick's own view is held per context and
// the in-between paint's is replaced by the midpoint of the previous tick's and this one's.
class ViewBlend final : public GuestCallProbes::Probe, public frame::AluConstantsListener {
  public:
    static constexpr uint32_t kUploadView = 0x02874074;
    static constexpr uint32_t kUploadViewFirstInstruction = 0x7c0802a6; // mfspr r0, LR
    static constexpr size_t kContextRegister = 3;
    static constexpr uint32_t kUploadedFlag = 0xb6;
    static constexpr uint32_t kWords = 12;
    // The probe sees GX2's write pointer at the packet header; the hook names the first data word.
    static constexpr uintptr_t kPacketHeaderBytes = 4;
    static constexpr size_t kMaxPending = 1024;
    static constexpr size_t kMaxContexts = 64;

    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry, uint32_t resume);
    using ReadGuest = std::function<const void*(uint32_t address, uint32_t size)>;
    // Display thread: whether the paint in progress is the in-between one.
    using InBetween = std::function<bool()>;

    ViewBlend(Register registerProbe, ReadGuest readGuest,
              CommandStreamIdentity::ReadPosition readPosition, InBetween inBetween);

    void install();

    void OnInstall(GuestCallProbes::Installation installation) override;
    // Display thread.
    void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;
    // Latte thread.
    bool onAluConstants(const LatteFrameHooks::AluConstants& constants) override;

    struct Report {
        uint64_t calls = 0;
        uint64_t alreadyUploaded = 0;
        uint64_t unreadable = 0;
        uint64_t withoutBuffer = 0;
        uint64_t pendingDropped = 0;
        uint64_t matched = 0;
        uint64_t wrongSize = 0;
        uint64_t held = 0;
        uint64_t lerped = 0;
        // Lerps whose two ends differ: the camera moved between the ticks.
        uint64_t moved = 0;
        uint64_t firstSight = 0;
        uint64_t refusedUnblendable = 0;
        uint64_t refusedForRoom = 0;
        uint64_t contexts = 0;
    };

    Report report() const;
    std::string json() const;

  private:
    struct Pending {
        uint32_t context = 0;
        bool inBetween = false;
        uint64_t sequence = 0;
    };

    void dropOldestPending();

    Register m_register;
    ReadGuest m_readGuest;
    CommandStreamIdentity::ReadPosition m_readPosition;
    InBetween m_inBetween;
    mutable std::mutex m_mutex;
    std::optional<GuestCallProbes::Installation> m_installation;
    std::map<uintptr_t, Pending> m_pending;
    std::map<uint32_t, std::array<float, kWords>> m_held;
    uint64_t m_sequence = 0;
    Report m_report;
};

} // namespace wiiuport::title
