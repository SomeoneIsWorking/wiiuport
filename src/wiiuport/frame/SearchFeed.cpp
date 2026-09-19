#include "wiiuport/frame/SearchFeed.h"

namespace wiiuport::frame {

SearchFeed::SearchFeed(interp::TransformSearch& search) : m_search(search) {
}

void SearchFeed::onFrameRecorded(const FrameRecording& recording) {
    m_search.observe(recording);
}

} // namespace wiiuport::frame
