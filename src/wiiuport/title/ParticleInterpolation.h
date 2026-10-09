#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"
#include "wiiuport/title/DrawInterpolation.h"

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's JPA particles in the in-between paint.
//
// Particles are calculated in the scene's draw (`calc3D`) and drawn by the paint, so the draw
// phase's midpoint inputs do not reach them. In the paint drawn from a tick's draw phase, each
// particle's global position and draw parameters are set to the midpoint of where the previous
// paint drew it and where it is now, just before its emitter draws, and put back when the next draw
// phase starts.
class ParticleInterpolation final : public DrawInterpolation::MidPaintListener {
  public:
    // JPADraw::draw (JPADraw in r3): draws one emitter's particles and children.
    static constexpr uint32_t kEmitterDraw = 0x0282bec8;
    static constexpr uint32_t kEmitterDrawFirst = 0x7c0802a6; // mflr r0
    static constexpr size_t kDrawRegister = 3;

    // JPADraw to its emitter; the emitter's active and child particle lists (JSUPtrList, head
    // first).
    static constexpr uint32_t kDrawEmitter = 0xc0;
    static constexpr std::array<uint32_t, 2> kParticleLists{0x1ac, 0x1b8};
    // JSUPtrLink: the object, then prev and next.
    static constexpr uint32_t kLinkObject = 0x0;
    static constexpr uint32_t kLinkNext = 0xc;
    // JPABaseParticle: mGlobalPosition, and mCurFrame, its age in frames.
    static constexpr uint32_t kGlobalPosition = 0x28;
    static constexpr uint32_t kPositionWords = 3;
    static constexpr uint32_t kAge = 0x78;
    // JPADrawParams, set by the emitter's calc: axis, scale out, x and y, alpha out, prm and env
    // colour (RGBA8), then the rotation angle (u16, high half) and its speed. In words.
    static constexpr uint32_t kDrawParams = 0x8c;
    static constexpr uint32_t kDrawParamWords = 14;
    static constexpr size_t kAxisWord = 0;
    static constexpr size_t kScaleWords = 3;
    static constexpr size_t kAlphaWord = 8;
    static constexpr size_t kPrmColorWord = 11;
    static constexpr size_t kEnvColorWord = 12;
    static constexpr size_t kRotationWord = 13;
    // A list longer than any emitter's pool is a misread, not particles.
    static constexpr uint32_t kMaxListLength = 4096;

    struct Seams {
        DrawInterpolation::Register registerProbe;
        DrawInterpolation::ReadWords readWords;
        DrawInterpolation::WriteWords writeWords;
    };

    explicit ParticleInterpolation(Seams seams);

    // Registers with the fork, before the title is linked.
    void install();

    // On the display thread.
    void onEmitterDraw(uint32_t draw);
    void onMidPaintBegin(uint64_t tick) override;
    void onMidPaintEnd() override;

    std::string json() const;

  private:
    class Entry final : public GuestCallProbes::Probe {
      public:
        explicit Entry(ParticleInterpolation& owner) : m_owner(owner) {
        }

        void OnInstall(GuestCallProbes::Installation installation) override;
        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;
        std::optional<GuestCallProbes::Installation> installation;

      private:
        ParticleInterpolation& m_owner;
    };

    // A particle as a paint drew it.
    // What a paint draws a particle from.
    struct Drawn {
        std::array<uint32_t, kPositionWords> position{};
        std::array<uint32_t, kDrawParamWords> params{};
    };

    struct Seen {
        uint64_t tick = 0;
        Drawn drawn;
        float age = 0.0f;
    };

    struct Restore {
        uint32_t particle = 0;
        Drawn drawn;
    };

    // Each particle in the emitter's lists, or nothing when a list does not read.
    std::optional<std::vector<uint32_t>> particlesOf(uint32_t draw);
    bool read(uint32_t particle, Drawn& drawn, uint32_t& age);
    bool write(uint32_t particle, const Drawn& drawn);
    void record(uint32_t particle);
    void blend(uint32_t particle);

    DrawInterpolation::Register m_register;
    DrawInterpolation::ReadWords m_readWords;
    DrawInterpolation::WriteWords m_writeWords;
    Entry m_emitterDraw{*this};

    mutable std::mutex m_mutex;
    bool m_inMidPaint = false;
    // The tick the last mid paint was drawn from; the paint after it draws that tick as it is.
    uint64_t m_tick = 0;
    std::map<uint32_t, Seen> m_seen;
    std::set<uint32_t> m_blended;
    std::vector<Restore> m_restores;

    uint64_t m_emitterDraws = 0;
    uint64_t m_particleBlends = 0;
    uint64_t m_particleFirstSeen = 0;
    uint64_t m_particlesRenewed = 0;
    uint64_t m_unblendable = 0;
    uint64_t m_unreadable = 0;
    uint64_t m_writeFailures = 0;
    uint64_t m_restored = 0;
};

} // namespace wiiuport::title
