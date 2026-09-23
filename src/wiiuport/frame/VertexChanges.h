#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace wiiuport::frame {

// Whether the title's draws read vertex bytes it rewrote since the frame
// before, by vertex shader. A uniform blend moves what uniforms place; the
// geometry the title writes into vertex buffers each frame -- an effect built
// on the CPU -- steps at the title's rate however the rest is blended, and
// this is where it shows.
//
// Each draw's vertex bytes are hashed, and a draw has changed when no draw of
// its vertex shader read the same bytes the frame before. That asks nothing
// of draw order, which moves as objects come and go: a mesh drawn every frame
// reads the same bytes wherever it falls.
//
// Draws whose vertex shader reads no uniforms are tallied always: nothing but
// their vertex bytes can move them, and they are a few in a hundred. The rest
// cost a few tens of megabytes a frame to hash, so they are tallied only
// over a census of frames asked for.
//
// Drawn on the rendering thread and read from the control channel's, so every
// member is taken under one lock.
class VertexChanges {
  public:
    // A census longer than this is a mistake: it hashes every draw's bytes.
    static constexpr uint32_t kMaxCensusFrames = 256;

    struct VertexShader {
        uint64_t baseHash;
        uint64_t auxHash;

        auto operator<=>(const VertexShader&) const = default;
    };

    struct Counts {
        uint64_t draws{0};
        // Draws in a frame after one the shader also drew in, and of them
        // those whose bytes no draw of the shader read in that frame.
        uint64_t compared{0};
        uint64_t changed{0};
        // The vertex bytes read to tell, which is what telling costs.
        uint64_t bytesHashed{0};
    };

    struct Census {
        uint32_t framesAsked{0};
        uint32_t framesTaken{0};
        std::map<VertexShader, Counts> byShader;
    };

    // One of the title's draws.
    void onDraw(const LatteFrameHooks::DrawPrepared& draw);
    // The frame is complete: its draws are compared with the frame before's.
    void onFrameComplete();
    // Tallies the draws that read uniforms over the next `frames` frames,
    // dropping the census before.
    void requestCensus(uint32_t frames);

    // Copies, since the start and of the latest census.
    std::map<VertexShader, Counts> withoutUniforms() const;
    Census census() const;

  private:
    // One class of draws' hashes and counts.
    class Tally {
      public:
        void add(const VertexShader& shader, uint64_t hash, uint64_t bytes);
        void compareFrame();
        void clear();

        const std::map<VertexShader, Counts>& counts() const {
            return m_counts;
        }

      private:
        std::map<VertexShader, Counts> m_counts;
        // Each shader's draws' hashes this frame, and the frame before's
        // sorted.
        std::map<VertexShader, std::vector<uint64_t>> m_frame;
        std::map<VertexShader, std::vector<uint64_t>> m_previous;
    };

    mutable std::mutex m_mutex;
    Tally m_withoutUniforms;
    Tally m_withUniforms;
    uint32_t m_censusAsked{0};
    uint32_t m_censusTaken{0};
};

} // namespace wiiuport::frame
