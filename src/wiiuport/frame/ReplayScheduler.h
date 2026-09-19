#pragma once

#include "wiiuport/frame/FrameReplayer.h"
#include "wiiuport/frame/RecordingObserver.h"

namespace wiiuport::frame {

// Runs the replayer at the one safe moment: after a frame has been published.
//
// It exists so the recorder stays a recorder and the replayer stays free of
// any opinion about when it runs. Everything it holds is a reference to an
// owner that outlives it.
class ReplayScheduler final : public FrameEndListener {
  public:
    explicit ReplayScheduler(FrameReplayer& replayer);

    void onFrameRecorded(const FrameRecording& recording) override;

  private:
    FrameReplayer& m_replayer;
};

} // namespace wiiuport::frame
