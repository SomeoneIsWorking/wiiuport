#pragma once

#include "wiiuport/interp/TransformSearch.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wiiuport::interp {

// The values blended draws of a shader held in common with other draws: what
// the title hands a whole pass alike, such as its view and projection, rather
// than any one object's own.
//
// An object that cannot be blended is still seen through the in-between
// frame's camera. Drawn wholly at N, it stands where N's camera put it amid a
// frame the other objects draw from between the two: the sea's tiles pull
// apart, each a camera step from its neighbours. So a value it held exactly as
// a blended draw of its shader did, at N-2 and at N both, is taken to be that
// shared value and drawn as the blended draw drew it. Both ends must agree
// bit for bit, which a value the object merely happens to share with another
// in one frame does not; and every blended draw holding it must have drawn
// the same, or none is taken.
//
// Rebuilt every frame; the storage is kept between frames. Pure.
class SharedValues {
  public:
    void clear();
    // A blended draw: its values at N-2 and N, and what it is drawn with.
    // Only the values it moved in between the two are kept: one the same at
    // both ends is drawn at N by every draw alike.
    void addBlended(const ShaderKey& shader, std::span<const float> twoBack,
                    std::span<const float> latest, std::span<const float> blended);
    // Done adding: sorts what lookups search.
    void index();

    // What the blended draws holding `twoBack` at N-2 and `latest` at N at
    // `position` drew there; none when no blended draw of `shader` held both,
    // or when those that did drew different values.
    std::optional<float> blendOf(const ShaderKey& shader, uint32_t position, float twoBack,
                                 float latest) const;

  private:
    struct Held {
        ShaderKey shader;
        uint32_t position;
        uint32_t twoBack;
        uint32_t latest;
        uint32_t blended;

        auto operator<=>(const Held&) const = default;
    };

    std::vector<Held> m_held;
};

} // namespace wiiuport::interp
