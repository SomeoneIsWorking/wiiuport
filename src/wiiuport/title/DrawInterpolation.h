#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's in-between picture, drawn by the title's own draw phase.
//
// With the logic gate in, each tick's draw phase runs with the camera's inputs and each executed
// actor's placement set to the midpoint of the previous tick's and this tick's, and put back
// afterwards; the skipped call's draw phase then draws this tick's. Shown in order: mid(n-1, n), n.
// The camera's matrices and the actors' model matrices are derived in the draw methods from these
// inputs, so the title computes everything downstream itself.
class DrawInterpolation {
  public:
    // fpcM_Management: a tick is running.
    static constexpr uint32_t kManagement = 0x025df948;
    static constexpr uint32_t kManagementFirst = 0x7c0802a6; // mflr r0
    // camera_draw (view_class in r3).
    static constexpr uint32_t kCameraDraw = 0x024ffc40;
    static constexpr uint32_t kCameraDrawFirst = 0x9421fe80; // stwu r1,-0x180(r1)
    // fopAc_Draw (fopAc_ac_c in r3).
    static constexpr uint32_t kActorDraw = 0x025d4654;
    static constexpr uint32_t kActorDrawFirst = 0x7c0802a6; // mflr r0
    // fpcDw_Handler's AfterOfDraw: every process has drawn.
    static constexpr uint32_t kAfterDraw = 0x025f03f0;
    static constexpr uint32_t kAfterDrawFirst = 0x7c0802a6; // mflr r0
    static constexpr size_t kProcessRegister = 3;

    // view_class from fovy: fovy, aspect, eye, center, up, then bank in the high half.
    static constexpr uint32_t kCameraInputs = 0xd4;
    static constexpr uint32_t kCameraWords = 12;
    static constexpr size_t kAspectWord = 1;
    static constexpr size_t kBankWord = 11;

    // fopAc_ac_c, TWW's layout 0x11c later: condition, then old, current and shape_angle.
    static constexpr uint32_t kCondition = 0x2e4;
    static constexpr uint32_t kNotExecuted = 0x2;
    static constexpr uint32_t kPlacement = 0x300;
    static constexpr uint32_t kPlacementWords = 12;
    static constexpr size_t kOldPosWord = 0;
    static constexpr size_t kCurrentPosWord = 5;
    static constexpr size_t kShapeAngleWord = 10;
    static constexpr size_t kPosWords = 3;

    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry, uint32_t resume);
    using ReadWords = std::function<bool(uint32_t address, uint32_t* words, uint32_t count)>;
    using WriteWords = std::function<bool(uint32_t address, const uint32_t* words, uint32_t count)>;
    using Gated = std::function<bool()>;

    struct Seams {
        Register registerProbe;
        ReadWords readWords;
        WriteWords writeWords;
        // Whether the logic gate is in: without it there is no in-between paint to draw.
        Gated gated;
    };

    explicit DrawInterpolation(Seams seams);

    // Registers with the fork, before the title is linked.
    void install();

    // On by default; off draws every tick's draw phase as it is, for comparison.
    void setEnabled(bool enabled);

    // What each probe reports, on the display thread.
    void onManagement();
    void onCameraDraw(uint32_t camera);
    void onActorDraw(uint32_t actor);
    void onAfterDraw();

    std::string json() const;

  private:
    enum class Event : uint8_t {
        Management,
        CameraDraw,
        ActorDraw,
        AfterDraw
    };

    class Entry final : public GuestCallProbes::Probe {
      public:
        Entry(DrawInterpolation& owner, Event event) : m_owner(owner), m_event(event) {
        }

        void OnInstall(GuestCallProbes::Installation installation) override;
        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;
        std::optional<GuestCallProbes::Installation> installation;

      private:
        DrawInterpolation& m_owner;
        Event m_event;
    };

    template <size_t N> struct Seen {
        // The tick it was seen in; ticks count from 1.
        uint64_t tick = 0;
        std::array<uint32_t, N> words{};
    };

    struct Restore {
        uint32_t address = 0;
        std::vector<uint32_t> words;
    };

    bool blendCamera(uint32_t camera, const std::array<uint32_t, kCameraWords>& words);
    bool blendActor(uint32_t actor, const std::array<uint32_t, kPlacementWords>& words);

    // The words a draw phase sees, and the tick's own to put back after it.
    struct Change {
        std::span<const uint32_t> blended;
        std::span<const uint32_t> original;
    };

    void write(uint32_t address, Change change);

    Register m_register;
    ReadWords m_readWords;
    WriteWords m_writeWords;
    Gated m_gated;
    Entry m_management{*this, Event::Management};
    Entry m_cameraDraw{*this, Event::CameraDraw};
    Entry m_actorDraw{*this, Event::ActorDraw};
    Entry m_afterDraw{*this, Event::AfterDraw};

    mutable std::mutex m_mutex;
    bool m_enabled = true;
    bool m_inTick = false;
    uint64_t m_tick = 0;
    std::map<uint32_t, Seen<kCameraWords>> m_cameras;
    std::map<uint32_t, Seen<2>> m_shapes;
    std::vector<Restore> m_restores;

    uint64_t m_ticks = 0;
    uint64_t m_cameraBlends = 0;
    uint64_t m_cameraFirstSeen = 0;
    uint64_t m_actorDraws = 0;
    uint64_t m_actorBlends = 0;
    uint64_t m_actorsNotExecuted = 0;
    uint64_t m_unblendable = 0;
    uint64_t m_unreadable = 0;
    uint64_t m_writeFailures = 0;
    uint64_t m_restored = 0;
};

} // namespace wiiuport::title
