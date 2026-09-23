#pragma once

#include "wiiuport/frame/RecordingObserver.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace wiiuport::frame {

// Hands a tool several consecutive frames' uniform assemblies, exactly as the
// recorder published them.
//
// Which submitted values belong to the same object from one tick to the next
// is a question about consecutive frames, and a single published frame cannot
// answer it. The frames are copied on the thread that publishes them, at the
// frame boundary, so a tool reading from another thread never sees a frame
// half replaced -- and they are consecutive by construction rather than
// whichever frames a poll happened to land on.
//
// Armed on request and idle otherwise: copying every frame for a reader that
// is not there would cost the thread that draws them.
class RecordingSnapshot final : public FrameEndListener {
  public:
    static constexpr size_t kMaxFrames = 16;
    static constexpr const char* kMagic = "WIIUREC1";

    // One frame of a snapshot, as the recorder published it.
    struct Frame {
        bool complete{false};
        std::vector<RecordedUniformAssembly> assemblies;
    };

    // False when `frames` is zero or above the cap, or a snapshot is already
    // being filled.
    bool arm(size_t frames);

    void onFrameRecorded(const FrameRecording& recording) override;

    // The last completed snapshot, framed for the wire:
    //   magic, u32 frames; per frame u32 complete, u32 assemblies; per
    //   assembly u64 baseHash, u64 auxHash, u32 stage, u32 sources,
    //   u32[sources], u32 floats, f32[floats]. Host byte order.
    // Empty when no snapshot has completed.
    std::string framed() const;

    // Reads back what framed() wrote, on the host that wrote it, so a
    // snapshot kept from the title can be planned again offline. Throws
    // std::invalid_argument naming what is wrong rather than returning the
    // frames it managed to read.
    static std::vector<Frame> parse(std::string_view framed);

    uint64_t snapshotsCompleted() const;

  private:
    mutable std::mutex m_mutex;
    size_t m_wanted{0};
    std::vector<Frame> m_filling;
    std::vector<Frame> m_ready;
    uint64_t m_completed{0};
};

} // namespace wiiuport::frame
