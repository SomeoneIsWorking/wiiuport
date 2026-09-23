#include "wiiuport/interp/NeighbourCheck.h"

namespace wiiuport::interp {

NeighbourCheck::NeighbourCheck(frame::FrameCapture& capture) : m_capture(capture) {
}

bool NeighbourCheck::arm() {
    bool expected = false;
    return m_requested.compare_exchange_strong(expected, true);
}

void NeighbourCheck::capture(size_t slot) {
    if (m_capture.armOnce(slot)) {
        ++m_captured;
    } else {
        m_failed = true;
    }
}

void NeighbourCheck::beforeInBetween(uint64_t tick) {
    if (m_stage == Stage::Idle && m_requested.load()) {
        m_stage = Stage::Before;
    } else if (m_stage == Stage::Following && tick != m_beforeTick + 1) {
        ++m_restarted;
        m_stage = Stage::Before;
    }
    m_running = m_stage;
    if (m_running == Stage::Before) {
        m_beforeTick = tick;
        m_failed = false;
        m_captured = 0;
    }
}

void NeighbourCheck::beforeInBetweenPresent() {
    if (m_running == Stage::Following) {
        capture(kInBetweenSlot);
    }
}

void NeighbourCheck::beforeGuestFrameCopied() {
    if (m_running == Stage::Before) {
        capture(kBeforeSlot);
    } else if (m_running == Stage::Following) {
        capture(kAfterSlot);
    }
}

void NeighbourCheck::afterTick() {
    switch (m_running) {
    case Stage::Idle:
        return;
    case Stage::Before:
        // A tick that never reached its copy leaves the capture armed for
        // whichever copy comes next, which is another frame.
        if (m_failed || m_captured != 1) {
            finish(false);
        } else {
            m_stage = Stage::Following;
        }
        break;
    case Stage::Following:
        finish(!m_failed && m_captured == 3);
        break;
    }
    m_running = Stage::Idle;
}

void NeighbourCheck::finish(bool completed) {
    if (completed) {
        ++m_completed;
    } else {
        ++m_refused;
    }
    m_stage = Stage::Idle;
    m_requested.store(false);
}

} // namespace wiiuport::interp
