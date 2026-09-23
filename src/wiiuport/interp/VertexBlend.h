#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/interp/ObjectBlend.h"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace wiiuport::interp {

// Where one draw's vertex values sit in its bytes: its buffers' sizes and
// strides and its attributes, as the fork described them.
struct VertexLayout {
    struct Buffer {
        uint32_t sizeInBytes;
        uint32_t stride;

        auto operator<=>(const Buffer&) const = default;
    };

    struct Attribute {
        uint32_t buffer;
        uint32_t offset;
        uint32_t sizeInBytes;
        uint8_t format;
        uint8_t endianSwap;

        auto operator<=>(const Attribute&) const = default;
    };

    std::vector<Buffer> buffers;
    std::vector<Attribute> attributes;

    static VertexLayout of(const LatteFrameHooks::DrawPrepared& draw);

    auto operator<=>(const VertexLayout&) const = default;
};

// What blending one draw's vertices came to.
enum class VertexOutcome : uint32_t {
    // Its float values lie strictly between the partner's and its own, and it
    // is drawn from them.
    Blended,
    // Its bytes are the partner's: nothing in its vertices moved.
    Unchanged,
    // The object it draws was not blended: no partner, so no vertices a frame
    // before to blend from. Drawn as the title drew it.
    NoPartner,
    // Its partner's draw has other buffers or attributes, or none of it.
    ShapeDiffers,
    // A value's blend would not lie strictly between the two frames': a move
    // of an ulp. Drawn as the title drew it.
    Outside,
    // Its vertices changed only in values that are not floats to blend.
    NotFloats,
    Count
};

inline constexpr size_t kVertexOutcomeCount = static_cast<size_t>(VertexOutcome::Count);
std::string_view vertexOutcomeName(VertexOutcome outcome);

// Blends a draw's vertex bytes at `t` from `before`, its partner's, to
// `after`, its own, into `out` (as long as `after`): every float attribute
// value whose bits differ and which is a number at both ends is lerped, the
// rest kept as `after` has them. Pure, so the shipping arithmetic is what a
// test checks.
VertexOutcome blendVertexBytes(const VertexLayout& layout, std::span<const std::byte> before,
                               std::span<const std::byte> after, float t, std::span<std::byte> out);

// Draws the vertices the title positions on the CPU each frame -- skinned
// characters, effects -- between the title's two frames, as ObjectBlend does
// for what uniforms place.
//
// Each of the guest's draws whose vertex shader reads uniforms has its vertex
// bytes copied, and is tied to the object its vertex-stage uniforms drew: the
// last vertex-stage assembly recorded before it. When the runtime replays
// that draw, the object's partner in N-1 -- the planner's -- names the draw
// a frame before, and its vertices are blended from that draw's to N's, value
// by value, where both are floats. A draw whose object has no partner has no
// vertices a frame before to blend from, and is drawn as the title drew it,
// counted.
//
// The replay re-issues the recorded frame, so its n-th draw is the
// recording's n-th, and a draw is placed by the uniform assemblies replayed
// before it -- the cursor ObjectBlend keeps. One out of step stops the blend
// for the rest of that frame, counted.
//
// The partners are known once a frame ends, so its vertices are blended then,
// on a thread of their own: the thread that replays is the thread that
// renders, which the title waits on. The replay waits only for what is not
// yet blended, and says how long.
class VertexBlend final : public frame::AssemblyRecordedListener,
                          public frame::DrawRecordedListener,
                          public frame::FrameEndListener,
                          public frame::VertexFilter {
  public:
    // Reads the plan and the replay's place from `objects`, which must be
    // notified of a frame's end before this is.
    VertexBlend(const ObjectBlend& objects, float t);
    ~VertexBlend() override;

    VertexBlend(const VertexBlend&) = delete;
    VertexBlend& operator=(const VertexBlend&) = delete;

    void onAssemblyRecorded(const frame::RecordedUniformAssembly& assembly) override;
    void onDrawRecorded(const LatteFrameHooks::DrawPrepared& draw) override;
    void onFrameRecorded(const frame::FrameRecording& recording) override;
    bool onRuntimeDraw(const LatteFrameHooks::DrawPrepared& draw,
                       LatteFrameHooks::VertexReplacements& replacements) override;

    // Draws of every armed replay by what their vertices came to, among those
    // whose vertex shader reads uniforms.
    uint64_t draws(VertexOutcome outcome) const {
        return m_outcomes[static_cast<size_t>(outcome)];
    }

    // Replayed draws out of step with the recording, and replays that stopped
    // for it.
    uint64_t replaysDiverged() const {
        return m_replaysDiverged;
    }

    // Replays whose frames were not the plan's: a frame ended while planning
    // was off, so the vertices held are not those the partners name.
    uint64_t replaysUnaligned() const {
        return m_replaysUnaligned;
    }

    // What keeping and blending vertices costs: bytes copied from the
    // guest's draws and the time the copies took, the time the blending
    // thread took over the frames, and the time replays waited for it.
    uint64_t bytesCopied() const {
        return m_bytesCopied;
    }

    std::chrono::nanoseconds copying() const {
        return m_copying;
    }

    std::chrono::nanoseconds blending() const {
        return std::chrono::nanoseconds{m_blendingNanoseconds.load()};
    }

    std::chrono::nanoseconds waiting() const {
        return m_waiting;
    }

  private:
    // One of the guest's draws in a frame.
    struct Draw {
        uint64_t vertexShaderBaseHash;
        uint64_t vertexShaderAuxHash;
        // Uniform assemblies recorded before it in its frame.
        uint32_t assembliesBefore;
        // The vertex-stage assembly it was drawn with, and its place among
        // the draws that share it; none for a draw not kept.
        std::optional<uint32_t> vertexEntry;
        uint32_t ordinal;
        VertexLayout layout;
        // Where each buffer's copy starts in the frame's bytes.
        std::vector<size_t> bufferStarts;
    };

    struct Frame {
        std::vector<Draw> draws;
        std::vector<std::byte> bytes;
        // Each buffer copied, by where the guest keeps it: draws sharing a
        // mesh share its copy.
        std::map<std::pair<const void*, uint32_t>, size_t> copied;
        // The draw of each (vertex entry, ordinal).
        std::map<std::pair<uint32_t, uint32_t>, size_t> byEntry;
        // ObjectBlend's frames ended when this one ended.
        uint64_t frameIndex{0};
        // Assemblies recorded so far, and the last vertex-stage one.
        uint32_t assemblies{0};
        std::optional<uint32_t> lastVertexEntry;
        uint64_t lastVertexShaderBaseHash{0};
        uint64_t lastVertexShaderAuxHash{0};

        void clear();
    };

    // A draw's bytes, one buffer after another, from a frame's copies.
    std::span<const std::byte> bufferBytes(const Frame& frame, const Draw& draw,
                                           size_t buffer) const;

    // One pair of draws' buffers read under one layout: draws of a mesh
    // drawn many times in a frame blend it once.
    struct PairKey {
        std::vector<size_t> before;
        std::vector<size_t> after;
        VertexLayout layout;

        auto operator<=>(const PairKey&) const = default;
    };

    struct PairBlend {
        VertexOutcome outcome;
        // Where its blended buffers start in m_blended.
        size_t start;
    };

    // A distinct pair's place in m_blended, laid out before any is blended
    // so the buffer never moves under a replay reading it.
    struct PairSlot {
        size_t start;
        std::optional<VertexOutcome> outcome;
    };

    // A draw of the latest frame to blend against its partner's, which
    // the frame before holds.
    struct Job {
        size_t draw;
        size_t partner;
    };

    // Ties each of the latest frame's kept draws to its partner's and hands
    // the blending to its thread.
    void startBlending();
    void blendHandedOver(std::stop_token stop);
    void waitUntilBlended();
    // Waits until the blending thread has come to the latest frame's draw
    // at `index`.
    void waitUntilBlendedThrough(size_t index);
    // Blends one pair into m_blended at `start`, its bytes laid out there.
    VertexOutcome blendPair(const Draw& partner, const Draw& drawn, size_t start);
    void count(VertexOutcome outcome);

    const ObjectBlend& m_objects;
    float m_t;
    Frame m_building;
    Frame m_latest;
    Frame m_previous;
    // Where the replay is in the latest frame's draws; the replay it was
    // armed for, by ObjectBlend's frames ended, and whether it fell out of
    // step.
    size_t m_replayDraw{0};
    uint64_t m_replayFrame{0};
    bool m_replayArmed{false};
    bool m_replayStopped{false};
    // The latest frame's blended buffers, each pair's one after another, the
    // distinct pairs, and what each draw came to; none for a draw not kept.
    // The blending thread works through the jobs in draw order, publishing
    // in m_blendedThrough the draws it has done, so a replay reads a draw's
    // blend as soon as it is made rather than once the frame's all are.
    std::vector<std::byte> m_blended;
    std::vector<std::optional<PairBlend>> m_drawBlends;
    std::vector<Job> m_jobs;
    std::map<PairKey, size_t> m_pairs;
    std::vector<PairSlot> m_pairSlots;
    std::vector<size_t> m_jobPairs;
    std::atomic<size_t> m_blendedThrough{0};
    // A changed pair's buffers, one after another, as blendVertexBytes reads
    // them; kept for their capacity.
    std::vector<std::byte> m_before;
    std::vector<std::byte> m_after;
    std::array<uint64_t, kVertexOutcomeCount> m_outcomes{};
    uint64_t m_replaysDiverged{0};
    uint64_t m_replaysUnaligned{0};
    uint64_t m_bytesCopied{0};
    std::chrono::nanoseconds m_copying{0};
    std::chrono::nanoseconds m_waiting{0};
    std::atomic<int64_t> m_blendingNanoseconds{0};

    std::mutex m_mutex;
    std::condition_variable_any m_handedOver;
    std::condition_variable m_blendedAll;
    bool m_jobHandedOver{false};

    // Last, so it starts after and stops before everything it uses.
    std::jthread m_thread;
};

} // namespace wiiuport::interp
