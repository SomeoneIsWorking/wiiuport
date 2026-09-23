#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wiiuport::interp {

// The values blended draws held in common with draws drawn at N: what the
// title hands a whole pass alike, such as its view and projection, or the
// light its shadows are cast and looked up by, rather than any one object's
// own.
//
// An object that cannot be blended is still seen through the in-between
// frame. Drawn wholly at N, it stands where N's camera put it amid a frame
// the other objects draw from between the two: the sea's tiles pull apart,
// each a camera step from its neighbours. And a pass's value must be one
// value in every draw that reads it: the shadow caster that draws the pier
// into the light's map and the pier looking itself up in it hold the light
// alike, and when one draws it half way and the other at N, the pier shadows
// itself. So a value a draw at N held exactly as a blended draw did, at N-2
// and at N both, is taken to be that shared value and drawn as the blended
// draw drew it -- whichever shader drew it, and wherever among its values:
// the light sits at a different place in each of the thirty shaders that
// read it. Both ends must agree bit for bit, which a value the object merely
// happens to share with another in one frame does not; and every blended
// draw holding it must have drawn the same, or none is taken.
//
// An object new at N -- come into view, or drawn by another shader as it
// nears -- has no N-2 to hold ends by, and drawn wholly at N the whole group
// of it jumps: the island's palms did as the camera turned. Its values are
// looked up by their value at N alone, and taken only where every draw that
// held that value at N drew it alike: blended draws that moved it, and those
// blended or held that drew it unmoved. A constant such as 0 or 1 is held
// unmoved by some draw and so is never taken; a value the pass's view put
// into every draw is drawn alike by all of them.
//
// Rebuilt every frame; the storage is kept between frames. Pure.
class SharedValues {
  public:
    void clear();
    // A draw drawn at N whose values at N-2 are known: what it moved is
    // looked up, so only blended values it holds are kept. All are added
    // before any blended draw.
    void addWanted(std::span<const float> twoBack, std::span<const float> latest);
    // A blended draw: its values at N-2 and N, and what it is drawn with.
    // Only the values it moved in between the two are kept: one the same at
    // both ends is drawn at N by every draw alike.
    void addBlended(std::span<const float> twoBack, std::span<const float> latest,
                    std::span<const float> blended);

    // A draw drawn at N with no draw at N-2: its values at N are looked up.
    // Added before any blended or held draw.
    void addWantedAtLatest(std::span<const float> latest);
    // A held draw, drawn with its values at N unmoved.
    void addHeld(std::span<const float> latest);

    // What the blended draws holding `twoBack` at N-2 and `latest` at N drew
    // there; none when no blended draw held both, or when those that did drew
    // different values.
    std::optional<float> blendOf(float twoBack, float latest) const;
    // What every blended or held draw holding `latest` at N drew there; none
    // when none held it, when they drew different values, or when they drew
    // it unmoved.
    std::optional<float> blendOfLatest(float latest) const;

  private:
    // What the blended draws holding a pair of ends drew, and whether they
    // all drew it.
    struct Held {
        uint32_t blended;
        bool agreed;
    };

    static uint64_t endsOf(float twoBack, float latest);
    // Counts what a draw drew for each value at N a draw new at N wants.
    void addDrawnAtLatest(std::span<const float> latest, std::span<const float> drawn);

    std::unordered_set<uint64_t> m_wanted;
    // By their ends: one entry per value however many draws hold it, where
    // an entry per draw, sorted, was the most of this frame-end work.
    std::unordered_map<uint64_t, Held> m_held;
    // By the value at N alone, for draws new at N.
    std::unordered_set<uint32_t> m_wantedAtLatest;
    std::unordered_map<uint32_t, Held> m_heldAtLatest;
};

} // namespace wiiuport::interp
