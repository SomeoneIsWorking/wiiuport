#pragma once

// What every object blend test drives the blend with: the title's draws as the
// recorder and the renderer hand them over, and the frames fed to it.

#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/interp/ObjectBlend.h"

#include <cstdint>
#include <vector>

namespace wiiuport::tests::object_blend {

// Half way between the title's two frames, as the product blends.
inline constexpr float kHalfway = 0.5f;
inline constexpr uint64_t kActorShader = 0xdddd;
inline constexpr uint64_t kPierShader = 0xeeee;
// A second pass drawing the same actor, as its outline.
inline constexpr uint64_t kOutline = 0xeeee;
// The two blocks one actor alternates between, as the title double-buffers
// them: A on even frames, B on odd.
inline constexpr uint32_t kBlockA = 0xf4001000;
inline constexpr uint32_t kBlockB = 0xf4081000;
inline constexpr uint32_t kOtherA = 0xf4002000;
inline constexpr uint32_t kOtherB = 0xf4082000;

struct Draw {
    uint32_t block;
    std::vector<float> values;
    uint64_t shader{kActorShader};
    // A second block at an address the title allocated for this frame alone.
    uint32_t freshBlock{0};
    uint32_t stage{0};
    bool writesColour{true};
    // A block every draw of a pass sources alike, as a shadow cascade's.
    uint32_t passBlock{0};
    // A stage that compares against a depth texture, as the light's look-up.
    bool looksUpDepthMap{false};

    std::vector<uint32_t> sources() const;
};

// One replayed draw as the renderer hands it over: its own buffer, sourced
// from the block the guest's frame N drew it from.
struct ReplayedDraw {
    std::vector<uint32_t> sources;
    std::vector<float> values;
    LatteFrameHooks::UniformAssembly assembly{};

    explicit ReplayedDraw(const Draw& draw);
};

frame::FrameRecording frameOf(const std::vector<Draw>& draws);

// Replays frame N as the product does -- every draw, in the order recorded --
// and returns what each draw uploaded.
std::vector<std::vector<float>> replay(interp::ObjectBlend& blend, const std::vector<Draw>& frame);

// One guest frame as the recorder hands it over: each draw as it is recorded,
// then the frame's end.
void record(interp::ObjectBlend& blend, const std::vector<Draw>& draws);

// Three frames fed to a planning blend and armed on the last.
void armAfter(interp::ObjectBlend& blend, const std::vector<Draw>& twoBack,
              const std::vector<Draw>& oneBack, const std::vector<Draw>& latest);

// One actor walking one unit a frame, and a second standing still.
std::vector<Draw> walkTwoBack();
std::vector<Draw> walkOneBack();
std::vector<Draw> walkLatest();

} // namespace wiiuport::tests::object_blend
