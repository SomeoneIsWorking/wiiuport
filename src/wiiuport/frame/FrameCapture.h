#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::frame {

// One captured image, owned by copy.
//
// The hook hands over bytes that do not outlive the callback, and the
// callback runs on the renderer's own thread while a tool asks for the image
// from another. Copying is what makes those two safe to be true at once.
struct CapturedImage {
    int width{0};
    int height{0};
    std::vector<uint8_t> rgb;

    bool empty() const {
        return rgb.empty();
    }
};

// Holds the most recent frame the runtime asked to see.
//
// The point of it is that a claim about what a replay drew can be checked
// against an image rather than argued from counters. Capture is one-shot and
// armed deliberately: a capture on every frame would make the replayed frame
// indistinguishable from the one after it.
class FrameCapture {
  public:
    // How a capture is armed, injected so a test drives this without a
    // renderer. Returns false when no capture could be armed.
    using Request = bool (*)(LatteFrameHooks::CaptureCallback callback);

    explicit FrameCapture(Request request);

    // False when the request was refused, which is reported rather than
    // leaving the caller waiting for an image that will never arrive.
    bool armOnce();

    // Empty until a capture has landed.
    CapturedImage lastImage() const;

    uint64_t capturesRequested() const;
    uint64_t capturesRefused() const;
    uint64_t imagesReceived() const;

    // A self-describing framing so a client that reads a truncated or stale
    // body refuses instead of rendering nonsense at the wrong dimensions.
    static constexpr const char* kMagic = "WIIUIMG1";
    static constexpr size_t kHeaderBytes = 16;
    std::string lastImageFramed() const;

  private:
    void receive(const LatteFrameHooks::FrameImage& image);

    Request m_request;
    mutable std::mutex m_mutex;
    CapturedImage m_last;
    uint64_t m_requested{0};
    uint64_t m_refused{0};
    uint64_t m_received{0};
};

} // namespace wiiuport::frame
