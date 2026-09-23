#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/interp/AssemblyKey.h"
#include "wiiuport/interp/KeyedFrame.h"
#include "wiiuport/interp/SharedTransforms.h"
#include "wiiuport/interp/SharedValues.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wiiuport::interp {

// The positions at which the title handed one value to every draw of a
// shader, in N-1 and in N-2 both: frame state, such as the view, not any
// object's own. Every candidate holds the same there, so it tells none
// from another, and it is blended from N-1 whichever is the partner.
struct FrameState {
    std::span<const uint8_t> oneBack;
    std::span<const uint8_t> twoBack;

    bool at(size_t index) const {
        return index < oneBack.size() && index < twoBack.size() && oneBack[index] != 0 &&
               twoBack[index] != 0;
    }
};

// Moves every object in an in-between frame to where it stood between the
// title's two frames, each by its own identity.
//
// The title double-buffers its uniform storage: an object writes one block on
// even frames and another on odd, so a block address in frame N comes back in
// N+2 and never in N+1. The same address is therefore the same object two
// frames apart, and that is how its draw in N-1 -- its partner -- is found:
// the draw the object's N-2..N midpoint lands on. The in-between frame is
// then the lerp from the partner to N at t, which needs nothing of how the
// object moved; N-2 only establishes who it is.
//
// An address can be reused by a different object between those two frames,
// and blending two objects draws a third that never existed: such an object's
// midpoint lands on nothing in N-1. It, and an object drawn from blocks N-2
// never sourced, is then looked for in N-2 by its values -- the same shader's
// draw nearest it -- and blended only if that draw's midpoint with N lands on
// a draw in N-1 in turn. A pool that hands its blocks to different objects
// as they come and go, as the sea's tiles are when the grid under the camera
// shifts, is identified that way; a different object found nearest lands at
// half its step, as it does by address. Partners are learned as block addresses
// -- A on even frames goes with B on odd -- so one draw's search pairs every draw of that object,
// and later frames derive the partner rather than search for it. A derived partner is re-checked
// every frame, so a wrong or stale pairing fails the check rather than passing silently. An object
// that fails, or has no N-2, is drawn as the title drew it and counted -- all but what one that
// fails shares with the blended draws of its shader, such as the pass's view, which it is drawn
// with as they are: seen through the in-between frame's camera, not N's.
//
// Frame state -- a value every draw of the shader held alike in N-1 and N-2,
// such as the view -- is blended but is no evidence of identity: it sways as
// the camera does, unevenly, and is the same in every candidate. An object
// that moved in none of its own values takes any candidate, since each draws
// the same.
//
// Only numbers are blended. A value that is not a finite normal float at both
// ends -- an integer in a float's clothing, a NaN -- is the later frame's.
//
// Planned draw by draw as the guest records them, against frames N-1 and N-2,
// which are already whole, so the frame's end only has to finish N's index.
// Single-threaded and pure: whoever feeds it owns when that happens.
class ObjectPlanner {
  public:
    // `t` is where the in-between frame sits on N-1..N.
    explicit ObjectPlanner(float t);

    // Keys a draw into the frame being built and, once two whole frames are
    // held, plans it. The first of a frame first indexes N for searching.
    void add(const frame::RecordedUniformAssembly& assembly);
    // The frame being built is whole: it becomes N. Its values are indexed
    // by the next add(), so the frame's end pays only for finding by key.
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
    // The entry of N-1 that is the same object as an entry of N: the one a
    // blended entry was blended from, or the one a held entry's learned
    // blocks name there. None where it is not known.
    std::optional<size_t> partnerOf(size_t entry) const;
    // The entry of N-2 that is the same object as an entry of N -- blended,
    // held or unverified -- which is what checks the partner. None where the
    // object has no draw there.
    std::optional<size_t> earlierOf(size_t entry) const;

    // What each object in a planned frame came to. Their sum is the objects
    // planned; none is dropped without a reason.
    enum class Outcome : uint32_t {
        // Moved, and its midpoint landed on its partner: drawn blended.
        Blended,
        // Unchanged since N-2: drawn as it was, which is the blend.
        Held,
        // No draw with its blocks in N-2, and none found there by its values
        // whose midpoint lands in N-1: new, or moved beyond telling.
        Unmatched,
        // Moved, but nothing in N-1 is where it passed through, from the draw
        // its blocks name in N-2 or from the one nearest its values. Drawn at
        // N in its own values, and in those it held as a blended draw of its
        // shader did -- the pass's view -- as that draw is (SharedValues).
        Unverified,
        // Its partner was found, but its blend would not lie strictly between
        // N-1 and N: nearer each than they are to each other. An object that
        // moved less than a float can halve is one, and is drawn at N.
        Outside,
        // Draws the light's map or looks it up, and is drawn at N: a draw
        // that writes depth alone renders a map a later draw samples (the
        // shadow map), and a pixel stage's values shade rather than place.
        // The map and its look-up must hold one light. The look-up holds it
        // multiplied into the camera, whose blend value by value is neither
        // the light's nor the camera's: measured on Wind Waker HD, a turning
        // camera with the casters blended, or the look-up, or both, put
        // the pier in its own shadow in the in-between frame, and with both
        // at N it was not.
        Shading,
        Count
    };
    static constexpr uint32_t kPixelStage = LatteFrameHooks::kPixelStageIndex;
    static constexpr size_t kOutcomeCount = static_cast<size_t>(Outcome::Count);
    static std::string_view outcomeName(Outcome outcome);

    using Outcomes = std::array<uint64_t, kOutcomeCount>;

    // What N's objects came to.
    const Outcomes& latestOutcomes() const {
        return m_plan.outcomes;
    }

    // What one of N's objects came to.
    Outcome outcomeOf(size_t entry) const {
        return m_plan.outcomeOf[entry];
    }

    // Held objects whose draw in N-1 their learned block pairs named: their
    // uniforms are drawn as N's, and their partner is what the vertex blend
    // draws their vertices from.
    uint64_t heldPartnersDerived() const {
        return m_heldPartnersDerived;
    }

    // Partners derived from learned block pairs and confirmed, against
    // partners that had to be searched for. The search is the expensive one.
    uint64_t partnersDerived() const {
        return m_partnersDerived;
    }

    uint64_t partnersSearched() const {
        return m_partnersSearched;
    }

    // Partners found only once the object itself was found in N-2 by its
    // values, because its blocks named something else there or nothing.
    uint64_t partnersReidentified() const {
        return m_partnersReidentified;
    }

    // What the searches cost: draws compared against an object, in N-2 to
    // find it by its values and in N-1 to find its partner. A search is
    // exact, so these, not the searches, are what a scene makes expensive.
    uint64_t reidentifyAttempts() const {
        return m_reidentifyAttempts;
    }

    uint64_t nearestCandidates() const {
        return m_nearestCandidates;
    }

    uint64_t partnerCandidates() const {
        return m_partnerCandidates;
    }

    // An object whose search found nothing is not searched for again until
    // this many frames later. Most never will be found: a flipbook
    // sprite stepping sixty degrees a frame, or a draw with no uniform blocks
    // whose only key is its place in the draw order. Searching for them every
    // frame was measured as seven tenths of the planning time.
    static constexpr uint64_t kSearchRetryInterval = 8;

    // Searches not run because the same object's search of that kind failed
    // recently.
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

    // Values of blended objects kept at the later frame because they were the
    // same at N-2 and N but not at N-1: flipping, not moving.
    uint64_t valuesAlternating() const {
        return m_valuesAlternating;
    }

    // Unverified objects drawn in the values they share with blended draws
    // of their shader, and how many values that was, over the unverified
    // objects planned.
    uint64_t unverifiedSharingValues() const {
        return m_unverifiedSharingValues;
    }

    uint64_t valuesShared() const {
        return m_valuesShared;
    }

    // Objects drawn at N -- unverified or unmatched -- whose matrices were
    // carried through the in-between camera (SharedTransforms), and how many
    // 4x4s that was.
    uint64_t unblendedCarried() const {
        return m_unblendedCarried;
    }

    uint64_t transformsCarried() const {
        return m_transformsCarried;
    }

    // Objects drawn at N with nothing shared and nothing carried: while the
    // camera moves, each stands where N's camera put it, a jump against the
    // world drawn between.
    uint64_t leftAtN() const {
        return m_leftAtN;
    }

    // The latest frame's entries left at N, in entry order.
    std::span<const uint32_t> leftAtNEntries() const {
        return m_plan.leftAtN;
    }

  private:
    // Where an entry's blend sits in a plan's floats; none when drawn as is.
    static constexpr uint32_t kNotBlended = UINT32_MAX;

    // One frame's blends, and what each of its objects came to.
    struct Plan {
        std::vector<uint32_t> blendedAt;
        // The partner's entry in N-1, by entry, blended or held; kNotBlended
        // where none is known.
        std::vector<uint32_t> partnerAt;
        // The object's entry in N-2, by entry, blended, held or unverified;
        // kNotBlended where none is known.
        std::vector<uint32_t> earlierAt;
        std::vector<float> floats;
        // What each entry came to, by entry.
        std::vector<Outcome> outcomeOf;
        // The entries left at N, in entry order.
        std::vector<uint32_t> leftAtN;
        Outcomes outcomes{};

        void clear();
    };

    // Plans the building frame's entry against N-1 and N-2.
    Outcome plan(size_t entry);
    // Draws the building frame's objects that are drawn at N -- unverified
    // or unmatched -- through the in-between camera, once every draw of it is
    // planned: in the values they share with its blended ones
    // (SharedValues), and in the matrices every blended draw of their shader
    // changed alike (SharedTransforms).
    void seeUnblendedThroughTheCamera();

    FrameState frameState(const ShaderKey& shader) const;

    // An object's draws in N-2 and N-1: where it stood two frames back, and
    // its partner -- none when it stood exactly where it stands in N.
    struct Found {
        size_t earlier;
        std::optional<size_t> partner;
    };

    // Finds the object whose draw in N is `after` in N-2 and N-1: by its
    // blocks first, `earlier` being the draw they name in N-2 if any, then by
    // its values. A failure waits kSearchRetryInterval frames before the
    // searches run again.
    std::optional<Found> findPartner(const AssemblyKey& key, std::optional<size_t> earlier,
                                     std::span<const float> after);
    // The partner of the object at `before` in N-2 derived from learned block
    // pairs, if they still pass: its midpoint lands on the draw, or the draw
    // is also the one nearest it by its values.
    std::optional<size_t> derivedPartner(const AssemblyKey& key, std::span<const float> before,
                                         std::span<const float> after);
    // The key of `key`'s draw in N-1 under the learned block pairs, if every
    // block it sourced is either paired or shared by both frames.
    std::optional<AssemblyKey> derivedKey(const AssemblyKey& key) const;
    // The same shader's draw in N-2 nearest `after` over the values that are
    // numbers in both, starting from `start`, the draw the object's blocks
    // name, which the search then only has to beat.
    std::optional<size_t> nearestEarlier(const AssemblyKey& key, std::span<const float> after,
                                         std::optional<size_t> start);
    // The same shader's draw in N-1 whose values the midpoint lands nearest,
    // within the tolerance: first agreeing in the values the object held
    // too, then, failing that, in the values it moved in alone. Both searches are exact, over N-2's
    // and N-1's trees of their draws (DrawTree).
    std::optional<size_t> searchPartner(const AssemblyKey& key, std::span<const float> before,
                                        std::span<const float> after);

    // What the searches in N-1 look for, placed in m_point: the midpoint in
    // the values the object moved in, and the values it held.
    struct SearchPoint {
        // How far from the point a partner may lie; unbounded when the object
        // moved in nothing.
        double limitSquared;
        bool held;
    };

    SearchPoint placeSearchPoint(const AssemblyKey& key, std::span<const float> before,
                                 std::span<const float> after);
    void learn(const AssemblyKey& key, const AssemblyKey& partner);
    // Indexes N's values, once, before anything searches it.
    void indexLatest();

    // Where the in-between frame sits on N-1..N.
    float m_t;
    // The frame the guest is drawing, and its plan.
    KeyedFrame m_building;
    Plan m_buildingPlan;
    // Whole frames, latest first: N, N-1, N-2, and the plan for N.
    std::array<KeyedFrame, 3> m_frames;
    Plan m_plan;
    size_t m_framesHeld{0};
    bool m_latestUnindexed{false};
    uint64_t m_framesPlanned{0};
    uint64_t m_heldPartnersDerived{0};
    std::unordered_map<uint32_t, uint32_t> m_blockPartner;
    // Key hash of an object whose search failed, and the planned frame it may
    // be searched for again.
    std::unordered_map<uint64_t, uint64_t> m_searchAgainAt;
    // The same for a search by values.
    std::unordered_map<uint64_t, uint64_t> m_reidentifyAgainAt;
    // Reused by every search: the point searched for.
    std::vector<double> m_point;
    // Reused every frame: what the blended draws share, and the shaders of
    // the unverified ones, sorted, which are all that is looked up.
    SharedValues m_shared;
    SharedTransforms m_transforms;
    std::vector<ShaderKey> m_unverifiedShaders;
    // Reused per object: which of its values came from SharedValues.
    std::vector<uint8_t> m_sharedAt;
    uint64_t m_unblendedCarried{0};
    uint64_t m_transformsCarried{0};
    uint64_t m_leftAtN{0};
    uint64_t m_unverifiedSharingValues{0};
    uint64_t m_valuesShared{0};
    uint64_t m_partnersDerived{0};
    uint64_t m_partnersSearched{0};
    uint64_t m_partnersReidentified{0};
    uint64_t m_reidentifyAttempts{0};
    uint64_t m_nearestCandidates{0};
    uint64_t m_partnerCandidates{0};
    uint64_t m_searchesDeferred{0};
    uint64_t m_valuesNotBlended{0};
    uint64_t m_valuesAlternating{0};
};

} // namespace wiiuport::interp
