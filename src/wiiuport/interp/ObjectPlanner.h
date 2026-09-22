#pragma once

#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/interp/AssemblyKey.h"
#include "wiiuport/interp/KeyedFrame.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wiiuport::interp {

// Moves every object in an in-between frame to where it stood between the
// title's two frames, each by its own identity.
//
// The title double-buffers its uniform storage: an object writes one block on
// even frames and another on odd, so a block address in frame N comes back in
// N+2 and never in N+1. The same address is therefore the same object two
// frames apart, which is what this blends between: the in-between frame at
// point t of N-1..N sits at (1 + t) / 2 of N-2..N, exact for an object moving
// steadily and needing no guess at which odd-frame block goes with which even
// one.
//
// An address can be reused by a different object between those two frames,
// and blending two objects draws a third that never existed. So the blend is
// checked against the frame in between: its midpoint has to land on the
// object's partner draw in N-1. Partners are learned as block addresses -- A
// on even frames goes with B on odd -- so one draw's search pairs every draw
// of that object, and later frames derive the partner rather than search for
// it. A derived partner is re-checked every frame, so a wrong or stale
// pairing fails the check rather than passing silently. An object that fails, or has no N-2, is
// drawn as the title drew it and counted.
//
// Only numbers are blended. A value that is not a finite normal float at both
// ends -- an integer in a float's clothing, a NaN -- is the later frame's.
//
// Planned draw by draw as the guest records them, against frames N-1 and N-2,
// which are already whole, so the frame's end only has to finish N's index.
// Single-threaded and pure: whoever feeds it owns when that happens.
class ObjectPlanner {
  public:
    // How far the midpoint may land from the partner, against how far the
    // object moved over the two frames. A partner that passed through the
    // middle lands at 0; one that is really a different object, or the same
    // one standing at either end, lands at 0.5. This is half way between the
    // two. Measured on the sea: the median lands at 0.024 and 92% of moving
    // objects within it.
    static constexpr float kPartnerTolerance = 0.25f;

    // `t` is where the in-between frame sits on N-1..N.
    explicit ObjectPlanner(float t);

    // Keys a draw into the frame being built and, once two whole frames are
    // held, plans it.
    void add(const frame::RecordedUniformAssembly& assembly);
    // The frame being built is whole: it becomes N.
    void endFrame();
    // Drops every frame held: three more are needed before a plan is ready.
    void forget();

    // Whether N was planned against two whole frames before it.
    bool ready() const {
        return m_framesHeld >= m_frames.size();
    }

    // N, the frame the plan is for.
    const KeyedFrame& latest() const {
        return m_frames[0];
    }

    // The values N's entry is drawn with in the in-between frame; empty when
    // it is drawn as the title drew it.
    std::span<const float> blendOf(size_t entry) const;

    // What each object in a planned frame came to. Their sum is the objects
    // planned; none is dropped without a reason.
    enum class Outcome : uint32_t {
        // Moved, and its midpoint landed on its partner: drawn blended.
        Blended,
        // Unchanged since N-2: drawn as it was, which is the blend.
        Held,
        // No draw with its identity in N-2: new, or its blocks moved.
        Unmatched,
        // Moved, but nothing in N-1 is where it passed through.
        Unverified,
        Count
    };
    static constexpr size_t kOutcomeCount = static_cast<size_t>(Outcome::Count);
    static std::string_view outcomeName(Outcome outcome);

    using Outcomes = std::array<uint64_t, kOutcomeCount>;

    // What N's objects came to.
    const Outcomes& latestOutcomes() const {
        return m_plan.outcomes;
    }

    // Partners derived from learned block pairs and confirmed, against
    // partners that had to be searched for. The search is the expensive one.
    uint64_t partnersDerived() const {
        return m_partnersDerived;
    }

    uint64_t partnersSearched() const {
        return m_partnersSearched;
    }

    // An object whose search found nothing is not searched for again until
    // this many frames later. Most never will be found: a flipbook
    // sprite stepping sixty degrees a frame, or a draw with no uniform blocks
    // whose only key is its place in the draw order. Searching for them every
    // frame was measured as seven tenths of the planning time.
    static constexpr uint64_t kSearchRetryInterval = 8;

    // Searches not run because the same object's search failed recently.
    uint64_t searchesDeferred() const {
        return m_searchesDeferred;
    }

    // Beyond this many learned block pairs the table is dropped and relearned,
    // so a title that cycles through its whole uniform heap cannot grow it
    // without bound. Far above one frame's blocks.
    static constexpr size_t kMaxBlockPairs = 1u << 16;

    // Changed values kept at the later frame because they are not numbers.
    uint64_t valuesNotBlended() const {
        return m_valuesNotBlended;
    }

  private:
    // Where an entry's blend sits in a plan's floats; none when drawn as is.
    static constexpr uint32_t kNotBlended = UINT32_MAX;

    // One frame's blends, and what each of its objects came to.
    struct Plan {
        std::vector<uint32_t> blendedAt;
        std::vector<float> floats;
        Outcomes outcomes{};

        void clear();
    };

    // Plans the building frame's entry against N-1 and N-2.
    Outcome plan(size_t entry);
    // Whether the object at `before` in N-2 and `after` in N has a partner in
    // N-1 its midpoint lands on. Derived from learned block pairs when they
    // still pass, searched among the same shader's draws otherwise.
    bool findPartner(const AssemblyKey& key, std::span<const float> before,
                     std::span<const float> after);
    // The key of `key`'s draw in N-1 under the learned block pairs, if every
    // block it sourced is either paired or shared by both frames.
    std::optional<AssemblyKey> derivedPartner(const AssemblyKey& key) const;
    // The same shader's draw in N-1 whose values the midpoint lands nearest,
    // within the tolerance. Tries the values the object moved in first and
    // abandons a candidate as soon as it is out of tolerance, so a shader
    // drawn a thousand times costs a few values per candidate, not all.
    std::optional<size_t> searchPartner(const AssemblyKey& key, std::span<const float> before,
                                        std::span<const float> after);
    void learn(const AssemblyKey& key, const AssemblyKey& partner);

    // (1 + t) / 2: the blend runs over N-2..N, twice as long as N-1..N and
    // starting a frame earlier.
    float m_s;
    // The frame the guest is drawing, and its plan.
    KeyedFrame m_building;
    Plan m_buildingPlan;
    // Whole frames, latest first: N, N-1, N-2, and the plan for N.
    std::array<KeyedFrame, 3> m_frames;
    Plan m_plan;
    size_t m_framesHeld{0};
    uint64_t m_framesPlanned{0};
    std::unordered_map<uint32_t, uint32_t> m_blockPartner;
    // Key hash of an object whose search failed, and the planned frame it may
    // be searched for again.
    std::unordered_map<uint64_t, uint64_t> m_searchAgainAt;
    // Reused by every search: which values to compare first.
    std::vector<uint32_t> m_searchOrder;
    uint64_t m_partnersDerived{0};
    uint64_t m_partnersSearched{0};
    uint64_t m_searchesDeferred{0};
    uint64_t m_valuesNotBlended{0};
};

} // namespace wiiuport::interp
