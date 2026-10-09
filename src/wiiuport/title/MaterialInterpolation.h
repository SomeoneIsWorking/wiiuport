#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"
#include "wiiuport/title/DrawInterpolation.h"

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's material animation (texture SRT, TEV and material colour) in the in-between draw
// phase.
//
// An actor plays each animation's frame controller in execute and, in its draw, hands the
// controller's frame to the animation's entry, which evaluates the materials from it. Each entry
// an actor's draw calls is remembered; in the next gated draw phase, before that actor draws, each
// of those controllers' frames is set to the midpoint of its last two ticks', put back once the
// draw phase is over. An actor that plays one in its draw would step it in both draw phases; for
// the gated one it is set half a step back before the play, so it plays to the midpoint, and put
// back after, so it steps once a tick.
class MaterialInterpolation final : public DrawInterpolation::DrawPhaseListener {
  public:
    // An animation's entry (the animation in r3, the frame in f1): the animation embeds its
    // J3DFrameCtrl at +0 and stores the frame there before evaluating.
    struct EntryPoint {
        uint32_t address = 0;
        // Where the animation holds its J3D animation, to tell it from whatever replaced it.
        uint32_t animation = 0;
    };

    // bpk (material colour), btk and btk by model data (texture SRT), brk and brk by model data
    // (TEV colour). btp and bva are discrete and bck is the joints, which the pose blend covers.
    static constexpr std::array<EntryPoint, 5> kEntryPoints{{{0x025e779c, 0x10},
                                                             {0x025e7fc4, 0x68},
                                                             {0x025e80d0, 0x68},
                                                             {0x025e83fc, 0x10},
                                                             {0x025e8480, 0x10}}};
    static constexpr uint32_t kEntryFirst = 0x7c0802a6; // mflr r0
    static constexpr size_t kAnimationRegister = 3;
    // mDoExt_baseAnm::play: J3DFrameCtrl::update, then whether it stopped (controller in r3).
    static constexpr uint32_t kPlay = 0x025e742c;
    static constexpr uint32_t kPlayFirst = 0x7c0802a6; // mflr r0

    // HD's J3DFrameCtrl: rate, frame, start and end (s16), loop (s16) and attribute (u8).
    static constexpr uint32_t kControllerWords = 4;
    static constexpr size_t kRateWord = 0;
    static constexpr size_t kFrameWord = 1;
    static constexpr uint32_t kFrame = 4 * kFrameWord;
    static constexpr size_t kStartEndWord = 2;
    static constexpr size_t kLoopAttributeWord = 3;
    static constexpr uint32_t kLoopAttribute = 2;

    struct Seams {
        DrawInterpolation::Register registerProbe;
        DrawInterpolation::ReadWords readWords;
        DrawInterpolation::WriteWords writeWords;
    };

    explicit MaterialInterpolation(Seams seams);

    // Registers with the fork, before the title is linked.
    void install();

    // On the display thread.
    void onEntry(uint32_t animationOffset, uint32_t animation);
    void onPlay(uint32_t controller);
    void onDrawPhaseBegin(uint64_t tick) override;
    void onDrawPhaseEnd() override;
    void onActorDraw(uint32_t actor) override;

    std::string json() const;

  private:
    class Play final : public GuestCallProbes::Probe {
      public:
        explicit Play(MaterialInterpolation& owner) : m_owner(owner) {
        }

        void OnInstall(GuestCallProbes::Installation installation) override;
        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;
        std::optional<GuestCallProbes::Installation> installation;

      private:
        MaterialInterpolation& m_owner;
    };

    class Entry final : public GuestCallProbes::Probe {
      public:
        Entry(MaterialInterpolation& owner, EntryPoint point) : m_owner(owner), m_point(point) {
        }

        void OnInstall(GuestCallProbes::Installation installation) override;
        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;
        std::optional<GuestCallProbes::Installation> installation;

      private:
        MaterialInterpolation& m_owner;
        EntryPoint m_point;
    };

    // An animation an actor's draw entered, and the J3D animation it held then.
    struct Animation {
        uint32_t address = 0;
        uint32_t animationOffset = 0;
        uint32_t j3dAnimation = 0;
    };

    struct Owned {
        uint64_t tick = 0;
        std::vector<Animation> animations;
    };

    struct Seen {
        uint64_t tick = 0;
        float frame = 0.0f;
    };

    struct Restore {
        uint32_t address = 0;
        uint32_t frame = 0;
    };

    void blend(const Animation& animation);

    DrawInterpolation::Register m_register;
    DrawInterpolation::ReadWords m_readWords;
    DrawInterpolation::WriteWords m_writeWords;
    std::array<Entry, kEntryPoints.size()> m_entries{
        Entry{*this, kEntryPoints[0]}, Entry{*this, kEntryPoints[1]}, Entry{*this, kEntryPoints[2]},
        Entry{*this, kEntryPoints[3]}, Entry{*this, kEntryPoints[4]}};
    Play m_play{*this};

    mutable std::mutex m_mutex;
    bool m_inDraw = false;
    uint64_t m_tick = 0;
    uint32_t m_actor = 0;
    std::map<uint32_t, Owned> m_owned;
    std::map<uint32_t, Seen> m_seen;
    // Each animation an actor's draw entered, and the last tick it was played in a draw phase.
    std::map<uint32_t, uint64_t> m_entered;
    std::map<uint32_t, uint64_t> m_playedInDraw;
    std::vector<Restore> m_restores;

    uint64_t m_entriesSeen = 0;
    uint64_t m_blends = 0;
    uint64_t m_wraps = 0;
    uint64_t m_firstSeen = 0;
    uint64_t m_jumps = 0;
    uint64_t m_replaced = 0;
    uint64_t m_drawPlays = 0;
    uint64_t m_unreadable = 0;
    uint64_t m_writeFailures = 0;
    uint64_t m_restored = 0;
};

} // namespace wiiuport::title
