#pragma once

#include "wiiuport/interp/DrawObjects.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>

namespace wiiuport::guest {

// The title's objects whose vertices its CPU writes each frame -- particles,
// sea waves, sky clouds and 3D lines -- by the vertex buffer each last wrote.
//
// A particle's quad carries nothing of its own -- four corners and the UVs
// every quad of its kind has -- and the title hands a pool of particles
// buffers as they come and go, so its vertices cannot say which particle a
// draw is. The title's code can (recovered in setsail's
// docs/render-state.md): its particle writers (JSystem's JPA draw executors
// and `dPa_ripplePcallBack::draw`) each write one particle's quad into one
// of the particle's own two vertex buffers and commit it, its sea-wave and
// sky-cloud writers do the same for each wave and cloud card, and its 3D
// lines flush each line's buffers through one helper. ParticleProbe,
// EnvironmentProbe and LineProbe watch those calls
// and record here, by the buffer, the object and the leading bytes it wrote
// there; a draw from that buffer is that object's while it reads those bytes.
class BufferWriters final : public interp::DrawObjects {
  public:
    // The bytes a draw is checked by: a quad as the writers leave it, four
    // corners of 20 bytes each, and the start of a line's longer mesh.
    static constexpr size_t kLeadingBytes = 80;
    using Leading = std::array<std::byte, kLeadingBytes>;

    // A probe installed in the title's code, once it is linked.
    void noteInstalled() {
        m_installed.fetch_add(1);
    }

    // A write whose object or vertex buffer could not be read.
    void noteUnreadable() {
        m_calls.fetch_add(1);
        m_unreadable.fetch_add(1);
    }

    // Keeps that `object` wrote `leading` at the start of `source`. Safe
    // from any thread.
    void record(const void* source, const interp::GuestObject& object, const Leading& leading);

    std::optional<interp::GuestObject> objectDrawn(const void* source,
                                                   std::span<const std::byte> bytes) const override;

    // How many of the probes that record here were installed.
    uint32_t installed() const {
        return m_installed.load();
    }

    // Writes observed, and those that recorded nothing.
    uint64_t calls() const {
        return m_calls.load();
    }

    uint64_t unreadable() const {
        return m_unreadable.load();
    }

    // Draws found a particle's, and draws from a buffer a particle wrote
    // whose bytes are not what it wrote there -- the buffer since given to
    // another draw.
    uint64_t identified() const {
        return m_identified.load();
    }

    uint64_t rewritten() const {
        return m_rewritten.load();
    }

  private:
    struct Written {
        interp::GuestObject object;
        Leading leading;
    };

    std::atomic<uint32_t> m_installed{0};
    std::atomic<uint64_t> m_calls{0};
    std::atomic<uint64_t> m_unreadable{0};
    mutable std::atomic<uint64_t> m_identified{0};
    mutable std::atomic<uint64_t> m_rewritten{0};
    mutable std::mutex m_mutex;
    std::unordered_map<const void*, Written> m_bySource;
};

} // namespace wiiuport::guest
