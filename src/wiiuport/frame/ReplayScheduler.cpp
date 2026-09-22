#include "wiiuport/frame/ReplayScheduler.h"

namespace wiiuport::frame {

ReplayScheduler::ReplayScheduler(FrameReplayer& replayer, FramePresenter& presenter,
                                 FrameCapture& capture)
    : m_replayer(replayer), m_presenter(presenter), m_capture(capture) {
}

bool ReplayScheduler::armNullDiff(bool redraw) {
    if (m_replayer.isArmed() || m_phase != Phase::Idle) {
        return false;
    }
    // Nothing is armed here. Which frame the two captures belong to is
    // decided at a frame end, on the thread that owns frame boundaries.
    m_redrawPending = redraw;
    m_phase = Phase::Requested;
    return true;
}

void ReplayScheduler::onFrameShown(const FrameRecording& recording) {
    switch (m_phase) {
    case Phase::Idle:
        if (m_replayer.replayIfArmed(recording) > 0) {
            m_presenter.presentNow();
        }
        return;

    case Phase::Requested:
        // Consumed by the swap that ends the next frame, which is the frame
        // the replay below will re-issue.
        m_capture.armOnce(kTitleSlot);
        if (m_redrawPending) {
            m_replayer.armOnce();
        }
        m_phase = Phase::Capturing;
        return;

    case Phase::Capturing:
        m_phase = Phase::Idle;
        if (m_redrawPending && m_replayer.replayIfArmed(recording) == 0) {
            // Nothing was redrawn, so presenting would show the title's own
            // frame and the comparison would pass by comparing an image
            // with itself.
            return;
        }
        m_capture.armOnce(kReplaySlot);
        ++m_nullDiffsCompleted;
        m_presenter.presentNow();
        return;
    }
}

} // namespace wiiuport::frame
