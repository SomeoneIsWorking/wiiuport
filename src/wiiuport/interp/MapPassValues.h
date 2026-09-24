#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>

namespace wiiuport::interp {

// What a pass hands every draw into a map alike -- the light a shadow map is
// cast by -- told from what each object draws into it with.
//
// A draw into the light's map (one that writes depth alone) holds two kinds
// of value: its object's placement, and the light's view and projection. The
// light's is looked up again by the pass that samples the map, and that
// look-up is drawn at N: a pixel stage's values shade rather than place, and
// the look-up holds the light multiplied into the camera, whose blend value by
// value is neither. So an object's shadow can move between the title's frames
// only if its draw into the map is blended in its own placement and holds the
// light at N, one light in the map and in its look-up. Measured on Wind Waker
// HD, a turning camera with the casters blended wholly put the pier in its own
// shadow in the in-between frame.
//
// Which values are the light is told by who holds them. Of 600 draws into the
// maps of one frame on the island, every moving one moved the light's rotation
// alike -- 378 draws -- and a cascade's projection alike, 126 draws each; a
// value an object moved was held by its own draws alone, one per cascade. So a
// value that moved from the same N-2 end to the same N end in draws of two
// different objects is the pass's, and is held at N. An object is named by its
// own block, the one fewest of the frame's map draws source: the cascade's
// block is sourced by every draw into that cascade. Objects that move as one,
// sharing a rotation, are taken for the pass too and their shadows step at the
// title's rate: holding too much is a shadow that steps, blending the light is
// a shadow in the wrong place.
//
// Rebuilt every frame; the storage is kept between frames. Pure.
class MapPassValues {
  public:
    void clear();

    // A draw into a map whose values at N-2 are known, by the object it
    // draws. All are added before any is held.
    void add(uint64_t object, std::span<const float> twoBack, std::span<const float> latest);

    // Whether every value a draw moved, it moved as a draw of another object
    // did: it moved only with the pass, and drawn at N is where it belongs.
    // False for a draw that moved nothing.
    bool movesOnlyThePass(std::span<const float> twoBack, std::span<const float> latest) const;

    // Puts back at N, in a blended draw into a map, every value it moved as
    // draws of another object moved it too. Returns how many it put back.
    size_t holdThePass(std::span<const float> twoBack, std::span<const float> latest,
                       std::span<float> blended) const;

  private:
    // The first object seen moving a value from one end to the other, and
    // whether another did too.
    struct Holder {
        uint64_t object;
        bool several;
    };

    std::unordered_map<uint64_t, Holder> m_moved;
};

} // namespace wiiuport::interp
