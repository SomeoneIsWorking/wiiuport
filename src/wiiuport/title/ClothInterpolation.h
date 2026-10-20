#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"
#include "wiiuport/title/DrawInterpolation.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's cloth (flags, banners, the sail) in the in-between draw phase.
//
// dCloth_packet_c::cloth_move (0x0251d4ec) steps the cloth's points once a tick, and its
// cloth_draw copies the current points and normals into the frame's vertex buffer (0x0251d864)
// in the draw phase. Each tick's own copy is recorded; in the gated draw phase the points and
// normals are set to the midpoint of the last tick's and this one's before the copy, and put back
// when the draw phase ends.
class ClothInterpolation final : public DrawInterpolation::DrawPhaseListener {
  public:
    // dCloth's vertex copy (packet in r3).
    static constexpr uint32_t kUpload = 0x0251d864;
    static constexpr uint32_t kUploadFirst = 0x9421ffc8; // stwu r1,-0x38(r1)
    static constexpr size_t kPacketRegister = 3;
    // dCloth_packet_c, TWW's layout 0x88 later: grid sizes, then points, normals and back normals,
    // two buffers each, the current one picked by the u8 at kCurrent.
    static constexpr uint32_t kFlyGrid = 0x98;
    static constexpr uint32_t kHoistGrid = 0x9c;
    static constexpr uint32_t kBuffers = 0xb0;
    static constexpr uint32_t kBufferKinds = 3;
    static constexpr uint32_t kCurrent = 0x1c0;
    // The GameCube plot indexes points by a byte.
    static constexpr uint32_t kMaxPoints = 256;

    struct Seams {
        DrawInterpolation::Register registerProbe;
        DrawInterpolation::ReadWords readWords;
        DrawInterpolation::WriteWords writeWords;
    };

    explicit ClothInterpolation(Seams seams);

    // Registers with the fork, before the title is linked.
    void install();

    // On the CPU thread, in either draw phase.
    void onUpload(uint32_t packet);
    void onDrawPhaseBegin(uint64_t tick) override;
    void onDrawPhaseEnd() override;
    void onActorDraw(uint32_t actor) override;

    std::string json() const;

  private:
    class Entry final : public GuestCallProbes::Probe {
      public:
        explicit Entry(ClothInterpolation& owner) : m_owner(owner) {
        }

        void OnInstall(GuestCallProbes::Installation installation) override;
        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;
        std::optional<GuestCallProbes::Installation> installation;

      private:
        ClothInterpolation& m_owner;
    };

    // A cloth's current points, normals and back normals as a draw phase copied them.
    struct Seen {
        uint64_t tick = 0;
        std::vector<uint32_t> buffers; // the current buffer of each kind
        std::vector<std::vector<uint32_t>> words;
    };

    struct Restore {
        uint32_t address = 0;
        std::vector<uint32_t> words;
    };

    bool read(uint32_t packet, Seen& seen);
    void blend(const Seen& previous, const Seen& current);

    DrawInterpolation::Register m_register;
    DrawInterpolation::ReadWords m_readWords;
    DrawInterpolation::WriteWords m_writeWords;
    Entry m_upload{*this};

    mutable std::mutex m_mutex;
    bool m_inDrawPhase = false;
    // The tick the last gated draw phase was drawn from; the draw phase after it draws that tick.
    uint64_t m_tick = 0;
    std::map<uint32_t, Seen> m_seen;
    std::vector<Restore> m_restores;
    // Buffers already blended this draw phase: cloths that share points share buffers.
    std::set<uint32_t> m_blended;

    uint64_t m_uploads = 0;
    uint64_t m_blends = 0;
    uint64_t m_firstSeen = 0;
    uint64_t m_shared = 0;
    uint64_t m_unblendable = 0;
    uint64_t m_unreadable = 0;
    uint64_t m_writeFailures = 0;
    uint64_t m_restored = 0;
};

} // namespace wiiuport::title
