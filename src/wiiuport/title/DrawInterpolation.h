#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's in-between picture, drawn by the title's own draw phase.
//
// With the logic gate in, each tick's draw phase runs with the camera's inputs, each executed
// actor's placement and each model's joint matrices at the midpoint of the previous tick's and this
// tick's, put back afterwards; the skipped call's draw phase then draws this tick's. Shown in order:
// mid(n-1, n), n. Everything downstream of these the title derives itself.
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
    // fpcDw_Handler's BeforeOfDraw: a draw phase starts.
    static constexpr uint32_t kBeforeDraw = 0x025f03c4;
    static constexpr uint32_t kBeforeDrawFirst = 0x7c0802a6; // mflr r0
    // fpcDw_Handler's AfterOfDraw: every process has drawn.
    static constexpr uint32_t kAfterDraw = 0x025f03f0;
    static constexpr uint32_t kAfterDrawFirst = 0x7c0802a6; // mflr r0
    // The model's view pass (model in r3): world matrices times the camera, only in a draw phase.
    static constexpr uint32_t kModelView = 0x027f55fc;
    static constexpr uint32_t kModelViewFirst = 0x9421ffe0; // stwu r1,-0x20(r1)
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

    // J3DModel::calc (0x027f4d5c) makes each joint's world matrix from the base TR matrix and the
    // animation, in execute for some actors and in the draw phase for most.
    static constexpr uint32_t kModelBase = 0xc8;
    static constexpr uint32_t kMatrixWords = 12;
    // Model to skeleton; the skeleton's world matrices and joint count (u16, high half).
    static constexpr uint32_t kModelSkeleton = 0x2c;
    static constexpr uint32_t kSkeletonWorld = 0x10;
    static constexpr uint32_t kSkeletonJoints = 0x2c;
    // A skeleton larger than any the title builds is a misread, not a model.
    static constexpr uint32_t kMaxJoints = 512;

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
    void onModelView(uint32_t model);
    void onBeforeDraw();
    void onAfterDraw();

    std::string json() const;

  private:
    enum class Event : uint8_t {
        Management,
        CameraDraw,
        ActorDraw,
        ModelView,
        BeforeDraw,
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
    // A model's base TR matrix, where its world matrices are, and the matrices.
    struct ModelPose {
        std::array<uint32_t, kMatrixWords> base{};
        uint32_t world = 0;
        std::vector<uint32_t> joints;
    };

    std::optional<ModelPose> readModel(uint32_t model);
    void recordModel(uint32_t model, ModelPose pose);
    bool blendModel(uint32_t model, const ModelPose& pose);

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
    Entry m_modelView{*this, Event::ModelView};
    Entry m_beforeDraw{*this, Event::BeforeDraw};
    Entry m_afterDraw{*this, Event::AfterDraw};

    mutable std::mutex m_mutex;
    bool m_enabled = true;
    bool m_inTick = false;
    bool m_inDraw = false;
    uint64_t m_tick = 0;
    std::map<uint32_t, Seen<kCameraWords>> m_cameras;
    std::map<uint32_t, Seen<2>> m_shapes;
    // Each model as the skipped call's draw phase drew it: the tick's own.
    struct SeenModel {
        uint64_t tick = 0;
        ModelPose pose;
        // Its base is set in the draw phase from an actor's placement, already at the midpoint.
        bool baseFromDraw = false;
    };
    std::map<uint32_t, SeenModel> m_models;
    // The base each model's tick drew with, to learn whether the base follows the draw's inputs.
    std::map<uint32_t, Seen<kMatrixWords>> m_tickBases;
    std::set<uint32_t> m_viewedInDraw;
    std::vector<Restore> m_restores;

    uint64_t m_ticks = 0;
    uint64_t m_cameraBlends = 0;
    uint64_t m_cameraFirstSeen = 0;
    uint64_t m_actorDraws = 0;
    uint64_t m_actorBlends = 0;
    uint64_t m_actorsNotExecuted = 0;
    uint64_t m_modelViews = 0;
    uint64_t m_modelBlends = 0;
    uint64_t m_modelFirstSeen = 0;
    uint64_t m_modelBasesFromDraw = 0;
    uint64_t m_unblendable = 0;
    uint64_t m_unreadable = 0;
    uint64_t m_writeFailures = 0;
    uint64_t m_restored = 0;
};

} // namespace wiiuport::title
