#include "wiiuport/frame/FrameCapture.h"

#include <cstring>

namespace wiiuport::frame {

FrameCapture::FrameCapture(Request request) : m_request(request) {
}

bool FrameCapture::armOnce() {
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_requested += 1;
    }
    // The callback outlives this call and runs on the renderer's thread.
    // `this` is a process-lifetime owner, which is the only reason capturing
    // it here is safe.
    auto armed = m_request([this](const LatteFrameHooks::FrameImage& image) {
        receive(image);
    });
    if (!armed) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_refused += 1;
    }
    return armed;
}

void FrameCapture::receive(const LatteFrameHooks::FrameImage& image) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_last.width = image.width;
    m_last.height = image.height;
    m_last.rgb.assign(image.rgb, image.rgb + image.byteCount);
    m_received += 1;
}

CapturedImage FrameCapture::lastImage() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_last;
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

std::string FrameCapture::lastImageFramed() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    std::string framed;
    framed.resize(kHeaderBytes + m_last.rgb.size());
    std::memcpy(framed.data(), kMagic, 8);
    auto width = static_cast<uint32_t>(m_last.width);
    auto height = static_cast<uint32_t>(m_last.height);
    std::memcpy(framed.data() + 8, &width, sizeof(width));
    std::memcpy(framed.data() + 12, &height, sizeof(height));
    if (!m_last.rgb.empty()) {
        std::memcpy(framed.data() + kHeaderBytes, m_last.rgb.data(), m_last.rgb.size());
    }
    return framed;
}

} // namespace wiiuport::frame
