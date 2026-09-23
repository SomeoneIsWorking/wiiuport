#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/interp/Midpoint.h"
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
    // Its vertices are as they were in N-2, whatever N-1 held: it stands
    // where it stood, and is drawn as the title drew it.
    Held,
    // Told from its partner's draw only by its place among identical draws,
    // and its vertices' N-2..N midpoint does not land on it: some other
    // mesh. Drawn as the title drew it.
    Unverified,
    // The object it draws was not blended: no partner, so no vertices a frame
    // before to blend from. Drawn as the title drew it.
    NoPartner,
    // Its partner's draw, or its own two frames back, has other buffers or
    // attributes, or there is none.
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

// One vertex shader's replayed draws by what their vertices came to.
struct ShaderVertexOutcomes {
    uint64_t shaderBaseHash;
    std::array<uint64_t, kVertexOutcomeCount> draws{};
};

// How far the partner's draw is known to be the same mesh.
enum class PartnerIdentity : uint8_t {
    // Its object is told apart by the blocks it sourced.
    ByBlocks,
    // Its object shares its shader and blocks with other draws of the frame
    // and is told apart only by its place among them, which the title
    // reorders: the vertices themselves must show it.
    ByPlace
};

// Blends a draw's vertex bytes at `t` from `before`, its partner's in N-1, to
// `after`, its own in N, into `out` (as long as `after`): the float attribute
// values that moved from `twoBack`, its draw in N-2, to N are lerped, where
// numbers at both ends, and the rest kept as `after` has them. A partner
// known only `ByPlace` is taken only once its vertices' N-2..N midpoint lands
// on it (Midpoint). Pure, so the shipping arithmetic is what a test checks.
VertexOutcome blendVertexBytes(const VertexLayout& layout, std::span<const std::byte> twoBack,
                               std::span<const std::byte> before, std::span<const std::byte> after,
                               float t, PartnerIdentity identity, std::span<std::byte> out);

// Whether a draw's vertices passed through `between` on their way from
// `twoBack` to `after`, over every float value the layout reads. Pure.
Midpoint vertexMidpoint(const VertexLayout& layout, std::span<const std::byte> twoBack,
                        std::span<const std::byte> between, std::span<const std::byte> after);

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
// The partner names a draw only as far as the uniforms tell objects apart. A
// held object's uniforms moved in nothing, so any candidate passes them, and
// many draws hand the same uniforms from the same blocks -- clouds, effect
// sprites -- told apart only by their place among each other, which the title
// reorders. So for such a draw the one a frame before is taken only if the
// object's own vertices, from its draw two frames back to N, pass through it:
// the planner's midpoint test, on vertices. One that does not is some other
// mesh, and blending towards it drew the clouds rearranged. Its place names
// no draw at all -- a frame before, the title may draw the same mesh under
// other blocks, far along the frame -- so its draws two frames back and a
// frame before are those of its vertex shader and layout its vertices at N
// most resemble, keeping most of their values bit for bit, then nearest, and
// the midpoint test checks that one pair: a step taken from another cloud has
// a midpoint any cloud between the two lands on, and among every candidate in
// turn one happens to stand half way. A draw whose blocks are its own keeps
// its planner's draws: its animation turning back fails the midpoint test
// while the mesh is its own.
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

    // Off, replays draw the vertices as the title drew them, while the
    // vertices are still kept, so turning it back on blends at once. Safe
    // from any thread.
    void setBlending(bool blending) {
        m_blendingEnabled.store(blending);
    }

    bool isBlending() const {
        return m_blendingEnabled.load();
    }

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

    // The same, vertex shader by vertex shader: which meshes step at the
    // title's rate, and why. Safe from any thread.
    std::vector<ShaderVertexOutcomes> drawsByShader() const;

    // Draws told apart only by their place whose draw two frames back or a
    // frame before their vertices found among their shader's draws, other
    // than the planner's.
    uint64_t partnersFoundByVertices() const {
        return m_partnersFoundByVertices.load();
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
        // The kept draws of each vertex shader (base, aux hash): an object
        // told apart only by its place may be drawn anywhere among them the
        // frame before, under other blocks.
        std::map<std::pair<uint64_t, uint64_t>, std::vector<size_t>> byShader;
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
        std::vector<size_t> twoBack;
        std::vector<size_t> before;
        std::vector<size_t> after;
        VertexLayout layout;
        PartnerIdentity identity;

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
    // the frame before holds, and the same object's draw two frames back.
    struct Job {
        size_t draw;
        size_t partner;
        size_t earlier;
        PartnerIdentity identity;
    };

    // Ties each of the latest frame's kept draws to its partner's and hands
    // the blending to its thread.
    void startBlending();
    void blendHandedOver(std::stop_token stop);
    void waitUntilBlended();
    // Waits until the blending thread has come to the latest frame's draw
    // at `index`.
    void waitUntilBlendedThrough(size_t index);
    // Blends one pair into m_blended at `start`, its bytes laid out there,
    // checked against the object's draw two frames back.
    VertexOutcome blendPair(const Draw& earlier, const Draw& partner, const Draw& drawn,
                            PartnerIdentity identity, size_t start);
    // A place-identified job's draws two frames back and a frame before:
    // those of its shader and layout its vertices most resemble, the
    // planner's where none more.
    void placeByVertices(Job& job);
    // Of `planned` and the draws of `drawn`'s shader and layout in `frame`,
    // the one `drawn` -- gathered into m_after -- most resembles.
    size_t mostResembling(const Draw& drawn, const Frame& frame, size_t planned);
    // A draw's buffers one after another into `into`, as the blend reads them.
    void gather(const Frame& frame, const Draw& draw, std::vector<std::byte>& into) const;
    // The draw a frame holds for an object's entry and a draw's place among
    // it, if it was kept and is laid out as `drawn` is.
    static std::optional<size_t> drawOf(const Frame& frame, size_t entry, const Draw& drawn);
    void count(uint64_t shaderBaseHash, VertexOutcome outcome);

    const ObjectBlend& m_objects;
    float m_t;
    Frame m_building;
    Frame m_latest;
    Frame m_previous;
    Frame m_twoBack;
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
    // The draws two frames back and a frame before found for each (N-2,
    // N-1, N) buffers the planner named for a place-identified draw this
    // frame: its passes share them.
    std::map<std::tuple<std::vector<size_t>, std::vector<size_t>, std::vector<size_t>>,
             std::pair<size_t, size_t>>
        m_placed;
    std::vector<std::byte> m_candidate;
    std::map<PairKey, size_t> m_pairs;
    std::vector<PairSlot> m_pairSlots;
    std::vector<size_t> m_jobPairs;
    std::atomic<size_t> m_blendedThrough{0};
    // A changed pair's buffers, one after another, as blendVertexBytes reads
    // them; kept for their capacity.
    std::vector<std::byte> m_twoBackBytes;
    std::vector<std::byte> m_before;
    std::vector<std::byte> m_after;
    std::array<uint64_t, kVertexOutcomeCount> m_outcomes{};
    mutable std::mutex m_byShaderMutex;
    std::map<uint64_t, std::array<uint64_t, kVertexOutcomeCount>> m_byShader;
    uint64_t m_replaysDiverged{0};
    uint64_t m_replaysUnaligned{0};
    uint64_t m_bytesCopied{0};
    std::chrono::nanoseconds m_copying{0};
    std::chrono::nanoseconds m_waiting{0};
    std::atomic<int64_t> m_blendingNanoseconds{0};
    std::atomic<uint64_t> m_partnersFoundByVertices{0};
    std::atomic<bool> m_blendingEnabled{true};

    std::mutex m_mutex;
    std::condition_variable_any m_handedOver;
    std::condition_variable m_blendedAll;
    bool m_jobHandedOver{false};

    // Last, so it starts after and stops before everything it uses.
    std::jthread m_thread;
};

} // namespace wiiuport::interp
