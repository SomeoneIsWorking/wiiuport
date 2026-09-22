#include "wiiuport/frame/FramePresenter.h"

namespace wiiuport::frame {

FramePresenter::FramePresenter(Submit submit) : m_submit(submit) {
}

void FramePresenter::onPresentObserved(const LatteFrameHooks::PresentArguments& present) {
    ++m_presentsObserved;
    if (present.targetsDrc) {
        ++m_presentsObservedDrc;
    }
    // A copy can carry both bits, and then it is the TV's as well.
    if (!present.targetsTv) {
        return;
    }
    m_lastTvPresent = present;
    ++m_presentsObservedTv;
}

bool FramePresenter::presentIfArmed() {
    if (!m_armed) {
        return false;
    }
    m_armed = false;
    return presentNow();
}

bool FramePresenter::presentNow() {
    if (!hasObservedPresent()) {
        ++m_presentsRefusedUnobserved;
        return false;
    }
    if (!m_submit(m_lastTvPresent)) {
        ++m_presentsRefusedBySubmit;
        return false;
    }
    ++m_presentsSubmitted;
    return true;
}

} // namespace wiiuport::frame
