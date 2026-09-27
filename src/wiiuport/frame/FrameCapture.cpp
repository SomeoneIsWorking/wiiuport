#include "wiiuport/frame/FrameCapture.h"

#include <atomic>
#include <cstring>
#include <memory>

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
    }, 1);
    if (!armed) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_refused += 1;
    }
    return armed;
}

bool FrameCapture::armRun(size_t count, size_t firstSlot) {
    if (count == 0) {
        return true;
    }
    if (firstSlot + count > kSlotCount) {
        // Named, because "nothing was captured" and "there was no room for the
        // run asked for" are different findings and a caller that treats them as
        // one waits for images that were never going to arrive.
        std::lock_guard<std::mutex> guard(m_mutex);
        m_requested += 1;
        m_refused += 1;
        return false;
    }
    // The fork's run re-arms as each image lands, and each re-arm routes the
    // next image to the next slot, so the pair ends up in two slots without this
    // side having to poll for the first before asking for the second.
    auto next = std::make_shared<std::atomic<size_t>>(0);
    auto armed = m_request(
        [this, firstSlot, next](const LatteFrameHooks::FrameImage& image) {
            const size_t index = next->fetch_add(1);
            if (firstSlot + index < kSlotCount) {
                receive(firstSlot + index, image);
            }
        },
        static_cast<int>(count));
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
