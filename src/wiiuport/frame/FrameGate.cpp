#include "wiiuport/frame/FrameGate.h"

namespace wiiuport::frame {

void FrameGate::pause() {
    std::lock_guard lock(m_mutex);
    if (m_released) {
        return;
    }
    m_status.paused = true;
    m_changed.notify_all();
}

void FrameGate::step(uint64_t frames) {
    std::lock_guard lock(m_mutex);
    if (m_released) {
        return;
    }
    m_status.paused = true;
    m_status.stepsLeft += frames;
    m_changed.notify_all();
}

void FrameGate::resume() {
    std::lock_guard lock(m_mutex);
    m_status.paused = false;
    m_status.stepsLeft = 0;
    m_changed.notify_all();
}

void FrameGate::release() {
    std::lock_guard lock(m_mutex);
    m_released = true;
    m_status.paused = false;
    m_status.stepsLeft = 0;
    m_changed.notify_all();
}

bool FrameGate::awaitHeld(std::chrono::milliseconds timeout) {
    std::unique_lock lock(m_mutex);
    return m_changed.wait_for(lock, timeout, [this] {
        return m_status.holding && m_status.stepsLeft == 0;
    });
}

FrameGate::Status FrameGate::status() const {
    std::lock_guard lock(m_mutex);
    return m_status;
}

bool FrameGate::runWhileHeld(const HeldJob& job) {
    std::unique_lock lock(m_mutex);
    if (!m_status.holding || m_status.stepsLeft > 0 || m_job != nullptr) {
        return false;
    }
    m_job = &job;
    m_changed.notify_all();
    m_changed.wait(lock, [this] {
        return m_job == nullptr;
    });
    return true;
}

void FrameGate::onFrameShown(const FrameRecording& recording) {
    std::unique_lock lock(m_mutex);
    if (m_status.paused && m_status.stepsLeft > 0) {
        --m_status.stepsLeft;
        ++m_status.framesStepped;
        m_changed.notify_all();
        if (m_status.stepsLeft > 0) {
            return;
        }
    }
    if (!m_status.paused) {
        return;
    }
    // The frame that just ended is the one held: a step's last frame ends
    // and the gate holds right after it, with that frame on the display.
    ++m_status.framesHeld;
    m_status.holding = true;
    m_changed.notify_all();
    while (true) {
        m_changed.wait(lock, [this] {
            return !m_status.paused || m_status.stepsLeft > 0 || m_job != nullptr;
        });
        if (m_job == nullptr) {
            break;
        }
        const HeldJob& job = *m_job;
        lock.unlock();
        job(recording);
        lock.lock();
        m_job = nullptr;
        m_changed.notify_all();
    }
    m_status.holding = false;
    m_changed.notify_all();
}

} // namespace wiiuport::frame
