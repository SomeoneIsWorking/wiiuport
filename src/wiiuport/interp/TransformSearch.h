#pragma once

#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/interp/Transform3x4.h"

#include <cstdint>
#include <map>
#include <vector>

namespace wiiuport::interp {

// Which shader's buffer a value was found in. Same shader and stage means the
// same buffer layout, so every comparison of one offset against another is
// only meaningful within one of these.
struct ShaderKey {
    uint64_t baseHash{0};
    uint64_t auxHash{0};
    uint32_t stageIndex{0};

    auto operator<=>(const ShaderKey&) const = default;
};

// One 3x4 found at one offset, with the evidence that it is a view.
//
// Two properties separate a camera from a moving object's world matrix, and
// both are measured rather than assumed: its rotation is a real rotation, and
// unrelated shaders carry the same value in the same frame. Every pass that
// draws the world is handed the same view; an object's transform reaches only
// the shaders that draw that object. A candidate found in one shader alone is
// reported as exactly that rather than quietly dropped or quietly promoted.
struct TransformCandidate {
    ShaderKey shader;
    uint32_t floatOffset{0};
    uint32_t framesSeen{0};
    uint32_t shadersSharing{1};
    float rotationError{0.0f};
    float meanTranslationStep{0.0f};
    Transform3x4 latest;

    bool isShared() const {
        return shadersSharing > 1;
    }
};

// Everything one search looked at, not only what it found.
struct SearchReport {
    std::vector<TransformCandidate> candidates;
    uint32_t framesObserved{0};
    size_t shadersTracked{0};
    size_t spansExamined{0};
    size_t rejectedVaryingWithinFrame{0};
    size_t rejectedNeverChanging{0};
    size_t rejectedRotation{0};
    // Counted over every candidate, not the listed subset, so a caller that
    // caps the list still reads an honest total. These two together are what
    // distinguishes a view from an object's transform, so the count of
    // candidates meeting both is the number worth gating on.
    size_t sharedAndMoving{0};
};

// Finds the title's view transform by watching what its values do, never from
// a recorded table of offsets: the offsets differ per shader and do not
// survive a shader cache change, but the behaviour does.
//
// A slot that varies between draws of one frame is some object's own
// transform. A slot that never varies at all is a constant, and reporting one
// as a camera is the easiest mistake available here. What remains -- constant
// across the frame, different between frames -- is the candidate set.
class TransformSearch {
  public:
    // Below this nothing is known to change, so frame-constant and invariant
    // cannot be told apart and no candidate means anything.
    static constexpr uint32_t kMinimumFrames = 2;

    // How far a row norm or row pair may stray from orthonormal. Measured
    // values sit within 1e-5, so this is loose enough for reduced precision
    // upstream and far tighter than a colour or projection row reaches by
    // accident.
    static constexpr float kDefaultRotationTolerance = 1e-3f;

    explicit TransformSearch(float rotationTolerance = kDefaultRotationTolerance);

    // Fold one published frame in. Every draw decides whether a slot varies
    // within the frame; the first draw of each shader is kept as that frame's
    // value.
    void observe(const frame::FrameRecording& recording);

    // Most shared first, then closest to a real rotation, with the
    // denominators alongside. An empty candidate list is otherwise
    // indistinguishable from never having looked, which is the failure this
    // search would report most convincingly and least usefully.
    SearchReport search() const;

    uint32_t framesObserved() const {
        return m_framesObserved;
    }

    size_t shadersTracked() const {
        return m_shaders.size();
    }

  private:
    // One shader's slots, folded across every frame seen so far. Held as
    // per-slot verdicts rather than retained buffers so the cost does not grow
    // with the number of frames watched.
    struct ShaderState {
        size_t width{0};
        std::vector<uint8_t> variesWithinFrame;
        std::vector<uint8_t> variesAcrossFrames;
        std::vector<float> currentFrame;
        std::vector<double> translationStepSum;
        uint32_t frames{0};
        uint32_t draws{0};
        bool hasPreviousFrame{false};
        std::vector<float> previousFrame;
    };

    void narrowTo(ShaderState& state, size_t width);
    void closeFrame(ShaderState& state);
    uint32_t countShadersSharing(const ShaderKey& exclude, const float* values) const;

    float m_rotationTolerance;
    uint32_t m_framesObserved{0};
    std::map<ShaderKey, ShaderState> m_shaders;
};

} // namespace wiiuport::interp
