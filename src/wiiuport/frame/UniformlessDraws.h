#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace wiiuport::frame {

// The title's draws whose vertex shader reads no uniforms, by vertex shader.
// Such a draw places its geometry from vertex data alone, so no uniform blend
// moves it: it is either still -- a full-screen pass -- or moved by the bytes
// the title writes into its vertex buffers each frame, an effect built on the
// CPU. Which of the two is told by those bytes: each draw's are hashed, and a
// frame's are compared with the same draw of the frame before, the same draw
// being the same occurrence of its vertex shader.
//
// Drawn on the rendering thread and read from the control channel's, so every
// member is taken under one lock; the draws counted here are a few in a
// hundred.
class UniformlessDraws {
  public:
    struct VertexShader {
        uint64_t baseHash;
        uint64_t auxHash;

        auto operator<=>(const VertexShader&) const = default;
    };

    struct Counts {
        uint64_t draws{0};
        // Of the draws, those whose vertex bytes differ from the same draw a
        // frame before, and those with no same draw a frame before -- the
        // shader drew fewer then, or it is the first frame seen.
        uint64_t changed{0};
        uint64_t unmatched{0};
        // The vertex bytes read to tell, which is what telling costs.
        uint64_t bytesHashed{0};
    };

    // One of the title's draws without vertex uniforms.
    void onDraw(const LatteFrameHooks::DrawPrepared& draw);
    // The frame is complete: its draws are compared with the frame before's.
    void onFrameComplete();
    // A copy, since the start.
    std::map<VertexShader, Counts> byShader() const;

  private:
    mutable std::mutex m_mutex;
    std::map<VertexShader, Counts> m_counts;
    // Each draw's vertex-byte hash in the order its shader drew it, this
    // frame and the one before.
    std::map<VertexShader, std::vector<uint64_t>> m_frame;
    std::map<VertexShader, std::vector<uint64_t>> m_previous;
};

} // namespace wiiuport::frame
