#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::frame {

// One captured image, owned by copy.
//
// The hook hands over bytes that do not outlive the callback, and the
// callback runs on a detached thread the renderer spawns at the swap while a
// tool asks for the image from another. Copying is what makes those two safe
// to be true at once.
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
    // Several images can be in flight at once -- a frame as the title
    // presented it and the same frame as a replay redrew it, or the title's
    // frames either side of an in-between frame and that frame -- and the renderer
    // delivers each from its own detached thread, so their arrival order is
    // not guaranteed. The destination is therefore chosen when the capture is
    // armed rather than when it lands.
    static constexpr size_t kSlotCount = 5;
    // How a capture is armed, injected so a test drives this without a
    // renderer. Returns false when no capture could be armed.
    using Request = bool (*)(LatteFrameHooks::CaptureCallback callback);

    explicit FrameCapture(Request request);

    // False when the request was refused, which is reported rather than
    // leaving the caller waiting for an image that will never arrive.
    bool armOnce(size_t slot = 0);

    // Empty until a capture has landed in that slot. An out-of-range slot is
    // empty too, which the callers all treat as "nothing captured".
    CapturedImage lastImage(size_t slot = 0) const;

    uint64_t capturesRequested() const;
    uint64_t capturesRefused() const;
    uint64_t imagesReceived() const;

    // A self-describing framing so a client that reads a truncated or stale
    // body refuses instead of rendering nonsense at the wrong dimensions.
    static constexpr const char* kMagic = "WIIUIMG1";
    static constexpr size_t kHeaderBytes = 16;
    std::string lastImageFramed(size_t slot = 0) const;

  private:
    void receive(size_t slot, const LatteFrameHooks::FrameImage& image);

    Request m_request;
    mutable std::mutex m_mutex;
    std::array<CapturedImage, kSlotCount> m_slots;
    uint64_t m_requested{0};
    uint64_t m_refused{0};
    uint64_t m_received{0};
};

} // namespace wiiuport::frame
