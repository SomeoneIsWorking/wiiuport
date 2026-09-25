#pragma once

#include "wiiuport/interp/DrawObjects.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>

namespace wiiuport::guest {

// The ripple rings on the water, by the particle each is.
//
// A ring's quad carries nothing of its own -- four world-space corners and
// the fixed UVs every ring has -- and the title hands a pool of them buffers
// as rings come and go, so its vertices cannot say which ring a draw is. The
// title's code can: Wind Waker HD draws each ripple particle through
// `dPa_ripplePcallBack::draw` (recovered in setsail's docs/render-state.md),
// which writes the quad into one of the particle's own two vertex buffers.
// RippleProbe watches that call and records here, by the buffer each
// particle wrote, the particle's address, age and position; a draw from that
// buffer is that particle's once its vertices are shown centred on it.
class RippleParticles final : public interp::DrawObjects {
  public:
    // The quad: four corners of position (x, y, z) and UV, big-endian
    // floats, written corner by corner around the particle.
    static constexpr size_t kCorners = 4;
    static constexpr size_t kCornerStride = 20;
    static constexpr size_t kQuadBytes = kCorners * kCornerStride;
    // The coordinates a corner is centred by, as float indices into it.
    static constexpr size_t kXFloat = 0;
    static constexpr size_t kZFloat = 2;

    // Where a particle was drawn: its identity and position.
    struct Drawn {
        interp::GuestObject object;
        float x;
        float z;
    };

    // Whether the probe was installed in the title's code, once it is linked.
    void noteInstalled(bool installed) {
        m_installed.store(installed);
    }

    // A draw call whose particle or vertex store could not be read.
    void noteUnreadable() {
        m_calls.fetch_add(1);
        m_unreadable.fetch_add(1);
    }

    // Keeps that the particle `drawn` wrote its quad to `source`. Safe from
    // any thread.
    void record(const void* source, const Drawn& drawn);

    std::optional<interp::GuestObject> objectDrawn(const void* source,
                                                   std::span<const std::byte> bytes) const override;

    // Whether `bytes` are a quad centred on (x, z): each pair of opposite
    // corners the particle's position less and plus one offset, to within
    // the rounding of each corner. Pure.
    static bool centredOn(std::span<const std::byte> bytes, float x, float z);

    bool installed() const {
        return m_installed.load();
    }

    // Draw calls observed, and those that recorded nothing.
    uint64_t calls() const {
        return m_calls.load();
    }

    uint64_t unreadable() const {
        return m_unreadable.load();
    }

    // Draws found a particle's, and those whose vertices were not centred on
    // the particle recorded there.
    uint64_t identified() const {
        return m_identified.load();
    }

    uint64_t offCentre() const {
        return m_offCentre.load();
    }

  private:
    std::atomic<bool> m_installed{false};
    std::atomic<uint64_t> m_calls{0};
    std::atomic<uint64_t> m_unreadable{0};
    mutable std::atomic<uint64_t> m_identified{0};
    mutable std::atomic<uint64_t> m_offCentre{0};
    mutable std::mutex m_mutex;
    std::unordered_map<const void*, Drawn> m_bySource;
};

} // namespace wiiuport::guest
