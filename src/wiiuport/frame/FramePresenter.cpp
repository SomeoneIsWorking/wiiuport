#include "wiiuport/frame/FramePresenter.h"

namespace wiiuport::frame {

FramePresenter::FramePresenter(Submit submit) : m_submit(submit) {
}

void FramePresenter::onPresentObserved(const LatteFrameHooks::PresentArguments& present) {
    m_lastPresent = present;
    ++m_presentsObserved;
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
    if (!m_submit(m_lastPresent)) {
        ++m_presentsRefusedBySubmit;
        return false;
    }
    ++m_presentsSubmitted;
    return true;
}

} // namespace wiiuport::frame
