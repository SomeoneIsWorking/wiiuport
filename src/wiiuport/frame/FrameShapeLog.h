#pragma once

#include "wiiuport/frame/RecordingObserver.h"

#include <cstddef>
#include <cstdint>
#include <deque>

namespace wiiuport::frame {

// What each of the last few published frames held.
//
// A single frame's totals cannot say whether the recorder is publishing whole
// frames or halves of them; a run of consecutive frames can. Measured on Wind
// Waker HD, the frame a replay re-issues holds 12 of the 211 shaders the
// search tracks, and this is what shows whether that 12 is every frame or
// every other one.
//
// It keeps shapes, never frames: a ring of recordings would hold megabytes for
// a diagnostic nobody is reading.
class FrameShapeLog final : public FrameEndListener {
  public:
    // Enough consecutive frames to see a pattern that alternates, or one that
    // repeats over a handful, without holding a run's worth of history.
    static constexpr size_t kDefaultDepth = 32;

    struct FrameShape {
        uint64_t frameIndex{0};
        size_t displayLists{0};
        size_t uniformAssemblies{0};
        size_t distinctShaders{0};
        size_t byteCount{0};
        bool complete{true};
    };

    explicit FrameShapeLog(size_t depth = kDefaultDepth);

    void onFrameRecorded(const FrameRecording& recording) override;

    // Oldest first. Empty means no frame has been published, which is a
    // different statement from a frame that held nothing.
    const std::deque<FrameShape>& shapes() const {
        return m_shapes;
    }

    uint64_t framesLogged() const {
        return m_framesLogged;
    }

  private:
    size_t m_depth;
    std::deque<FrameShape> m_shapes;
    uint64_t m_framesLogged{0};
};

} // namespace wiiuport::frame
