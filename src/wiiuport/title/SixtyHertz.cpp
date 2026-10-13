#include "wiiuport/title/SixtyHertz.h"

#include "lucent/log.h"

#include <utility>

namespace wiiuport::title {

SixtyHertz::SixtyHertz(Seams seams) : m_seams(std::move(seams)) {
}

SixtyHertz::~SixtyHertz() {
    {
        std::scoped_lock lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void SixtyHertz::start() {
    m_thread = std::thread([this] {
        run();
    });
}

void SixtyHertz::onPaint() {
    {
        std::scoped_lock lock(m_mutex);
        if (m_painted) {
            return;
        }
        m_painted = true;
    }
    m_wake.notify_all();
}

std::string SixtyHertz::switchOn() {
    std::string refusal = m_seams.enablePaint();
    if (!refusal.empty()) {
        return "the second paint: " + refusal;
    }
    refusal = m_seams.enableGate();
    if (refusal.empty()) {
        return {};
    }
    std::string undone = m_seams.disablePaint();
    return "the logic gate: " + refusal +
           (undone.empty() ? "" : "; and the second paint stays in: " + undone);
}

void SixtyHertz::run() {
    {
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [this] {
            return m_painted || m_stopping;
        });
        if (m_stopping) {
            return;
        }
    }
    std::string refusal = switchOn();
    lucent::info("sixty", "{}", refusal.empty() ? "on" : "off, refused by " + refusal);
}

} // namespace wiiuport::title
