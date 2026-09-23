#pragma once

#include <cstdint>
#include <optional>
#include <span>
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
    // Done adding: sorts what lookups search.
    void index();

    // What the blended draws holding `twoBack` at N-2 and `latest` at N drew
    // there; none when no blended draw held both, or when those that did drew
    // different values.
    std::optional<float> blendOf(float twoBack, float latest) const;

  private:
    struct Held {
        uint64_t ends;
        uint32_t blended;

        auto operator<=>(const Held&) const = default;
    };

    static uint64_t endsOf(float twoBack, float latest);

    std::unordered_set<uint64_t> m_wanted;
    std::vector<Held> m_held;
};

} // namespace wiiuport::interp
