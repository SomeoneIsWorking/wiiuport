#pragma once

#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/interp/TransformSearch.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace wiiuport::interp {

// Follows the view from one frame to the next, cheaply enough to run on every
// frame the title draws.
//
// The search finds the view by comparing every span of every shader against
// every other, which is the right way to find it and far too much work to
// repeat per frame. Once it is found the question each frame is narrower:
// what do the shaders that carried it last frame carry at the same place now,
// and which shaders of this frame carry that value. Both answers come from
// this frame alone, so the two endpoints of a blend are exactly the frame the
// guest just finished and the one before it -- never the last frame some
// particular shader happened to draw in.
//
// When the carriers stop agreeing, stop drawing, or stop holding a rotation,
// the view is lost and the full search is asked again, at a bounded rate: a
// scene with no view at all (a menu) must not pay for a search every frame.
class ViewTracker final : public frame::FrameEndListener {
  public:
    // How often a lost view is looked for again, in frames.
    static constexpr uint32_t kReseedInterval = 8;

    explicit ViewTracker(const TransformSearch& search,
                         float rotationTolerance = TransformSearch::kDefaultRotationTolerance);

    // After the search has folded the same frame in, so a re-seed reads this
    // frame's values rather than the last one's.
    void onFrameRecorded(const frame::FrameRecording& recording) override;

    // The view in the frame just recorded and in the one before, with every
    // slot of this frame that carries it. Empty unless both frames were
    // tracked: a blend needs two consecutive values of the same view.
    const std::optional<std::vector<ViewSlot>>& pair() const {
        return m_pair;
    }

    // Denominators for a view that is not there: frames where the tracked
    // view held, frames where it was lost, and how many searches were run to
    // find it again.
    uint64_t framesTracked() const {
        return m_framesTracked;
    }

    uint64_t framesLost() const {
        return m_framesLost;
    }

    uint64_t reseedsRun() const {
        return m_reseedsRun;
    }

    uint64_t reseedsFound() const {
        return m_reseedsFound;
    }

  private:
    struct Carrier {
        ShaderKey shader;
        uint32_t floatOffset{0};
    };

    // The value this frame's carriers agree on, when enough of them do.
    std::optional<Transform3x4> followCarriers(const frame::FrameRecording& recording) const;
    std::optional<Transform3x4> reseed();
    void lose();

    const TransformSearch& m_search;
    float m_rotationTolerance;
    std::vector<Carrier> m_carriers;
    std::optional<Transform3x4> m_view;
    std::optional<std::vector<ViewSlot>> m_pair;
    uint32_t m_framesSinceReseed{kReseedInterval};
    uint64_t m_framesTracked{0};
    uint64_t m_framesLost{0};
    uint64_t m_reseedsRun{0};
    uint64_t m_reseedsFound{0};
};

} // namespace wiiuport::interp
