#include "wiiuport/frame/FrameReplayer.h"

namespace wiiuport::frame {

FrameReplayer::FrameReplayer(Submit submit) : m_submit(submit) {
}

uint64_t FrameReplayer::replayIfArmed(const FrameRecording& recording) {
    if (!m_armed) {
        return 0;
    }
    m_armed = false;
    // An incomplete recording is not replayed. It would draw a frame missing
    // whatever went over the budget, which looks like a rendering fault rather
    // than the recording failure it is.
    if (!recording.isComplete()) {
        return 0;
    }
    m_replaysRun++;
    uint64_t submitted = 0;
    for (const RecordedDisplayList& list : recording.displayLists()) {
        if (m_submit(list.data.data(), static_cast<uint32_t>(list.data.size()))) {
            submitted++;
            continue;
        }
        m_listsRefused++;
    }
    m_listsSubmitted += submitted;
    return submitted;
}

} // namespace wiiuport::frame
