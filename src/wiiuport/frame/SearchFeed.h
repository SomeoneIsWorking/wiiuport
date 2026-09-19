#pragma once

#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/interp/TransformSearch.h"

namespace wiiuport::frame {

// Folds every published frame into a transform search.
//
// It carries no policy of its own -- every complete frame is evidence, and
// the search decides what to do with it. It exists so the search stays a pure
// analysis that a test can drive frame by frame, and so the recorder keeps
// knowing nothing about what reads it.
class SearchFeed final : public FrameEndListener {
  public:
    explicit SearchFeed(interp::TransformSearch& search);

    void onFrameRecorded(const FrameRecording& recording) override;

  private:
    interp::TransformSearch& m_search;
};

} // namespace wiiuport::frame
