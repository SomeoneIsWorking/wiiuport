#include "wiiuport/interp/RestoreCheck.h"

namespace wiiuport::interp {

RestoreCheck::RestoreCheck(frame::FramePresenter& presenter, frame::FrameCapture& capture)
    : m_presenter(presenter), m_capture(capture) {
}

bool RestoreCheck::arm(Against against) {
    State expected = State::Idle;
    State wanted = against == Against::Restored ? State::ArmedRestored : State::ArmedInBetween;
    return m_requested.compare_exchange_strong(expected, wanted);
}

void RestoreCheck::capture(size_t slot) {
    if (!m_capture.armOnce(slot)) {
        m_failed = true;
    }
}

void RestoreCheck::beforeInBetween(uint64_t /*tick*/) {
    m_running = m_requested.load();
    if (m_running == State::Idle) {
        return;
    }
    m_failed = false;
    m_checkCaptured = false;
    capture(kGuestSlot);
    if (!m_presenter.presentNow()) {
        m_failed = true;
    }
}

void RestoreCheck::beforeInBetweenPresent() {
    if (m_running == State::ArmedInBetween) {
        capture(kCheckSlot);
        m_checkCaptured = true;
    }
}

void RestoreCheck::beforeGuestFrameCopied() {
    if (m_running == State::ArmedRestored) {
        capture(kCheckSlot);
        m_checkCaptured = true;
    }
}

void RestoreCheck::afterTick() {
    if (m_running == State::Idle) {
        return;
    }
    // A tick that never reached the copy it meant to capture leaves the
    // capture armed for whichever copy comes next, which is another frame.
    if (m_failed || !m_checkCaptured) {
        ++m_refused;
    } else {
        ++m_completed;
    }
    m_running = State::Idle;
    m_requested.store(State::Idle);
}

} // namespace wiiuport::interp
