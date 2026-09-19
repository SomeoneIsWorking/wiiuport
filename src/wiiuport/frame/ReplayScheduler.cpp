#include "wiiuport/frame/ReplayScheduler.h"

namespace wiiuport::frame {

ReplayScheduler::ReplayScheduler(FrameReplayer& replayer) : m_replayer(replayer) {
}

void ReplayScheduler::onFrameRecorded(const FrameRecording& recording) {
    m_replayer.replayIfArmed(recording);
}

} // namespace wiiuport::frame
