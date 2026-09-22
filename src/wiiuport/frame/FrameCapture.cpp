#include "wiiuport/frame/FrameCapture.h"

#include <cstring>

namespace wiiuport::frame {

FrameCapture::FrameCapture(Request request) : m_request(request) {
}

bool FrameCapture::armOnce(size_t slot) {
    if (slot >= kSlotCount) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_requested += 1;
        m_refused += 1;
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_requested += 1;
    }
    // The callback outlives this call and runs on a thread the renderer
    // detaches at the swap. `this` is a process-lifetime owner, which is the
    // only reason capturing it here is safe.
    auto armed = m_request([this, slot](const LatteFrameHooks::FrameImage& image) {
        receive(slot, image);
    });
    if (!armed) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_refused += 1;
    }
    return armed;
}

void FrameCapture::receive(size_t slot, const LatteFrameHooks::FrameImage& image) {
    std::lock_guard<std::mutex> guard(m_mutex);
    CapturedImage& destination = m_slots[slot];
    destination.width = image.width;
    destination.height = image.height;
    destination.rgb.assign(image.rgb, image.rgb + image.byteCount);
    m_received += 1;
}

CapturedImage FrameCapture::lastImage(size_t slot) const {
    if (slot >= kSlotCount) {
        return {};
    }
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_slots[slot];
}

uint64_t FrameCapture::capturesRequested() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_requested;
}

uint64_t FrameCapture::capturesRefused() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_refused;
}

uint64_t FrameCapture::imagesReceived() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_received;
}

std::string FrameCapture::lastImageFramed(size_t slot) const {
    CapturedImage image = lastImage(slot);
    std::string framed;
    framed.resize(kHeaderBytes + image.rgb.size());
    std::memcpy(framed.data(), kMagic, 8);
    auto width = static_cast<uint32_t>(image.width);
    auto height = static_cast<uint32_t>(image.height);
    std::memcpy(framed.data() + 8, &width, sizeof(width));
    std::memcpy(framed.data() + 12, &height, sizeof(height));
    if (!image.rgb.empty()) {
        std::memcpy(framed.data() + kHeaderBytes, image.rgb.data(), image.rgb.size());
    }
    return framed;
}

} // namespace wiiuport::frame
