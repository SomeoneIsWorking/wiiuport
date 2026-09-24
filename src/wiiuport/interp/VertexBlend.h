#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/interp/Midpoint.h"
#include "wiiuport/interp/ObjectBlend.h"
#include "wiiuport/interp/SlotPool.h"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
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
    // Becomes `of(draw)` in place, keeping its vectors' storage.
    void assign(const LatteFrameHooks::DrawPrepared& draw);
    // Whether `of(draw)` would equal this, without building it: asked of
    // every replayed draw.
    bool describes(const LatteFrameHooks::DrawPrepared& draw) const;

    // Takes in the attributes another reader of the same buffers fetches;
    // false, and unchanged, when it lays the buffers out otherwise.
    bool absorb(const VertexLayout& other);

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
    // Everything it moved in by N stood bit for bit still from N-2 to N-1:
    // three frames hold no step to check the move against, and a mesh the
    // title parked out of sight and placed, or handed to another object, is
    // not told from one setting off. Blended half way, a quad parked under
    // the sea and placed in the sky was drawn on the water. Drawn as the
    // title drew it.
    Started,
    // A shader reading its buffers was excluded over the control channel,
    // to tell which draws a defect in the in-between frame is. Drawn as the
    // title drew it.
    Excluded,
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
// mesh by mesh on threads of their own: the thread that replays is the thread
// that renders, which the title waits on. The replay waits only for the mesh
// it draws, if that is not yet blended, and says how long.
class VertexBlend final : public frame::AssemblyRecordedListener,
                          public frame::DrawRecordedListener,
                          public frame::FrameEndListener,
                          public frame::VertexFilter {
  public:
    // Threads the meshes are blended on. While Link walked the island, one
    // thread took 11.7 ms of each frame's 33 and the replay, which draws the
    // meshes in the order they are blended, waited 7 ms of it: the guest fell
    // to 26 frames a second. Meshes blend independently, so a few threads
    // take a fraction of that each, and the replay's first meshes are done
    // soonest.
    static constexpr size_t kBlendWorkers = 4;

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

    // Draws every mesh the vertex shaders with these base hashes read as the
    // title drew it, by all its readers; none excludes nothing. A
    // maintainer's discriminator, not a setting. Safe from any thread.
    void exclude(std::vector<uint64_t> shaderBaseHashes) {
        std::lock_guard lock(m_excludedMutex);
        m_excluded = std::move(shaderBaseHashes);
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
    // title's rate, and why, as of the last frame's end. Safe from any
    // thread.
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
    // threads took over the frames, summed, and the time replays waited for
    // them.
    uint64_t bytesCopied() const {
        return m_bytesCopied;
    }

    std::chrono::nanoseconds copying() const {
        return m_copying;
    }

    std::chrono::nanoseconds blending() const {
        return m_pool.busy();
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
        // The mesh it reads, by the frame's meshes: draws reading the same
        // copies share one.
        uint32_t mesh{0};
        // The next kept draw with its vertex entry, if any.
        uint32_t nextOfEntry{0};
    };

    // The kept draws of one vertex entry: the first, the last, and how many,
    // chained through Draw::nextOfEntry.
    struct EntryDraws {
        uint32_t first{0};
        uint32_t last{0};
        uint32_t count{0};
    };

    // Hashes a pair of keys, for the per-draw lookups of a frame: the two
    // hashes combined as boost::hash_combine does.
    struct PairHash {
        template <typename First, typename Second>
        size_t operator()(const std::pair<First, Second>& key) const {
            size_t first = std::hash<First>{}(key.first);
            return first ^ (std::hash<Second>{}(key.second) + 0x9e3779b97f4a7c15ull + (first << 6) +
                            (first >> 2));
        }
    };

    // A frame's draws and their copies. Cleared, it keeps its storage: every
    // kept draw's vectors allocated afresh and freed each frame cost as much
    // as copying the vertices.
    struct Frame {
        std::span<const Draw> draws() const {
            return {drawSlots.data(), drawCount};
        }

        // The next draw's slot, emptied, its vectors keeping their storage.
        Draw& appendDraw();

        std::vector<Draw> drawSlots;
        size_t drawCount{0};
        std::vector<std::byte> bytes;
        // Each buffer copied, by where the guest keeps it: draws sharing a
        // mesh share its copy.
        std::unordered_map<std::pair<const void*, uint32_t>, size_t, PairHash> copied;
        // The kept draws of each vertex entry, by entry: a hash map of
        // (entry, ordinal) allocated a node for every kept draw of the frame.
        std::vector<EntryDraws> entryDraws;
        // The kept draws of each vertex shader (base, aux hash): an object
        // told apart only by its place may be drawn anywhere among them the
        // frame before, under other blocks.
        std::unordered_map<std::pair<uint64_t, uint64_t>, std::vector<size_t>, PairHash> byShader;
        // Each mesh's id, by the copies it reads.
        std::map<std::vector<size_t>, uint32_t> meshes;
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

    struct PairBlend {
        VertexOutcome outcome;
        // Where its blended buffers start in m_blended.
        size_t start;
    };

    // A draw of the latest frame to blend against its partner's, which
    // the frame before holds, and the same object's draw two frames back.
    struct Job {
        size_t draw;
        size_t partner;
        size_t earlier;
        PartnerIdentity identity;
        // The draw of its mesh whose partner was taken, and every attribute
        // the mesh's readers at N fetch: all of them draw the same bytes.
        // The layout is a draw's of the latest frame or one of
        // m_meshLayouts, both kept until the blend of the frame is done.
        size_t plannedBy;
        const VertexLayout* layout;
    };

    // The layout every reader of a mesh fetches together: the first
    // reader's until another fetches more, then a merged copy; none once
    // two readers lay its buffers out otherwise.
    struct MeshLayout {
        const VertexLayout* layout{nullptr};
        bool merged{false};
        bool refused{false};
    };

    // A mesh's job and its place in m_blended, laid out before any is
    // blended so the buffer never moves under a replay reading it. Every
    // draw of a mesh shares its job (startBlending), so a mesh is one pair
    // of draws and is blended once, however often it is drawn. Its outcome
    // is written by the worker that blends it, and read once the pool says
    // the slot is done.
    struct PairSlot {
        Job job;
        size_t start;
        VertexOutcome outcome{VertexOutcome::Blended};
    };

    // A blending thread's storage: a pair's buffers one after another, as
    // blendVertexBytes reads them, and a candidate's as mostResembling does.
    // Kept for their capacity.
    struct Scratch {
        std::vector<std::byte> twoBack;
        std::vector<std::byte> before;
        std::vector<std::byte> after;
        std::vector<std::byte> candidate;
    };

    // Ties each of the latest frame's kept draws to its partner's, lays each
    // mesh's slot out and hands the slots to the pool.
    void startBlending();
    // The latest frame's kept draw at `index` tied to its partner's, or why
    // it has none.
    std::variant<Job, VertexOutcome> planDraw(size_t index) const;
    // Blends the mesh of a slot, on a worker.
    void blendSlot(size_t slot, Scratch& scratch);
    // What the latest frame's draw at `index` came to, waiting for its mesh
    // if a worker is still blending it; none for a draw not kept.
    std::optional<PairBlend> blendOf(size_t index);
    // Blends one pair into m_blended at `start`, its bytes laid out there,
    // checked against the object's draw two frames back.
    VertexOutcome blendPair(const Draw& earlier, const Draw& partner, const Draw& drawn,
                            const VertexLayout& layout, PartnerIdentity identity, size_t start,
                            Scratch& scratch);
    // A place-identified job's draws two frames back and a frame before:
    // those of its shader and layout its vertices most resemble, the
    // planner's where none more. Done as its mesh is blended, not ahead of
    // every mesh, so the replay's first draws do not wait on the frame's
    // every search.
    void placeByVertices(Job& job, Scratch& scratch);
    // Of `planned` and the draws of `drawn`'s shader with its buffers in
    // `frame`, the one `drawn` -- gathered into scratch.after -- most
    // resembles under `layout`.
    size_t mostResembling(const Draw& drawn, const VertexLayout& layout, const Frame& frame,
                          size_t planned, Scratch& scratch) const;
    // A draw's buffers one after another into `into`, as the blend reads them.
    void gather(const Frame& frame, const Draw& draw, std::vector<std::byte>& into) const;
    // The draw a frame holds for an object's entry and a draw's place among
    // it, if it was kept and is laid out as `drawn` is.
    static std::optional<size_t> drawOf(const Frame& frame, size_t entry, const Draw& drawn);
    void count(uint64_t shaderBaseHash, VertexOutcome outcome);
    void publishCounts();

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
    // The latest frame's blended buffers, each pair's one after another;
    // what each draw not blended came to, and the slot of each that is. The
    // slots are in the order the replay first draws their meshes, so it
    // reads a mesh's blend as soon as it is made rather than once the
    // frame's all are.
    std::vector<std::byte> m_blended;
    std::vector<std::optional<PairBlend>> m_drawBlends;
    std::vector<std::optional<size_t>> m_drawSlots;
    // The latest frame's meshes: their layouts, the merged ones' storage,
    // whether a reader's shader was excluded, the job all their draws share,
    // and their slots. Kept for their capacity.
    std::vector<MeshLayout> m_meshLayouts;
    std::deque<VertexLayout> m_mergedLayouts;
    std::vector<uint8_t> m_meshExcluded;
    std::vector<std::optional<Job>> m_meshJobs;
    std::vector<std::optional<size_t>> m_meshSlots;
    std::vector<PairSlot> m_pairSlots;
    std::vector<Scratch> m_scratch{kBlendWorkers};
    std::array<uint64_t, kVertexOutcomeCount> m_outcomes{};
    // Counted on the rendering thread without a lock, and published under
    // one once a frame: a lock and a map search per replayed draw was a
    // fifth of the replay's own vertex work.
    std::unordered_map<uint64_t, std::array<uint64_t, kVertexOutcomeCount>> m_byShaderPending;
    mutable std::mutex m_byShaderMutex;
    std::map<uint64_t, std::array<uint64_t, kVertexOutcomeCount>> m_byShader;
    uint64_t m_replaysDiverged{0};
    uint64_t m_replaysUnaligned{0};
    uint64_t m_bytesCopied{0};
    std::chrono::nanoseconds m_copying{0};
    std::chrono::nanoseconds m_waiting{0};
    std::atomic<uint64_t> m_partnersFoundByVertices{0};
    std::atomic<bool> m_blendingEnabled{true};
    std::mutex m_excludedMutex;
    std::vector<uint64_t> m_excluded;

    // Last, so its threads start after and stop before everything they use.
    SlotPool m_pool;
};

} // namespace wiiuport::interp
