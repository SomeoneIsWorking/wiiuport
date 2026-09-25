#pragma once

#include "wiiuport/interp/Transform3x4.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wiiuport::interp {

// The rows of a stage that looks up the light's map which carry the light
// into the camera, and their values for the in-between camera.
//
// A stage comparing against the light's map applies its look-up to positions
// seen from the camera: each row is the light seen through the view, the
// light at N multiplied by the view at N's inverse. The in-between frame draws
// the map with the light at N and its positions through the in-between view,
// so a row that is drawn at N looks the map up where the positions stood seen
// from N: on a fast swing the pier's top fell wholly into shadow. The exact
// row is the row at N times the view at N times the in-between view's
// inverse. The light turns with the camera in Wind Waker HD, so a row is not
// told by being the same seen through the view at N-2 and at N.
//
// A row is told by the map instead. Seen back through the view at N, every
// look-up row of shader `62ae9bc6` lies in a plane of two of the light's
// axes -- a row along its depth axis, or one mixing an axis with the depth a
// projection divides by -- and the draws into the map at N hold those axes as
// a rotation with no translation. The light's rotation is told from others a
// map draw holds by having moved since N-2 and by being the one most of the
// frame's map draws hold alike -- 378 of 600 on the island, where an object's
// own is held by one draw a cascade. Taken for the light's, an object's
// identity, or the rotation of one turning about the vertical, puts the
// world's upright axis among the light's, and a scalar the stage keeps in a
// row's first value, or a fog row every stage shares, then lay in a plane of
// two of them for any camera that does not roll and was turned as a
// direction. A row the camera does not carry, or one that
// lies in such a plane by chance but does not move, is left as it is. A row
// whose fourth value stood bit for bit still while the rest turned is a
// direction, whose fourth value is not a translation: it is turned alone.
// Pure, so the shipping arithmetic is what a test checks.
class LightLookUp {
  public:
    // Rows are a rotation when unit length and at right angles to within
    // this much.
    static constexpr float kAxisTolerance = 1e-3f;
    // A row lies in a plane of two axes when its direction's part along the
    // third is at most this.
    static constexpr float kPlaneTolerance = 1e-4f;

    enum class RowForm : uint8_t {
        // Not the light seen through the camera: drawn as the title drew it.
        Unrelated,
        // A direction: turned from the view at N to the in-between view.
        Direction,
        // A plane of the light's space: moved as a point is.
        Affine,
    };

    void clearAxes();
    // Counts the rotations a draw into the map holds that moved from N-2 to
    // N. Every draw is added before the axes are chosen.
    void addMapValues(std::span<const float> twoBack, std::span<const float> latest);
    // The light's axes: the rotation that moved held by the most draws, two
    // at least. None when two rotations are held by as many.
    void chooseAxes();

    size_t axisCount() const {
        return m_axes.size();
    }

    // What a row of four the stage held at N-2 and holds at N is.
    RowForm classify(std::span<const float> twoBack, std::span<const float> latest,
                     const Transform3x4& viewAtN) const;

    // The row at N as the in-between view sees it.
    static void rebase(RowForm form, std::span<const float> latest, const Transform3x4& viewAtN,
                       const Transform3x4& viewBetween, std::span<float> out);

    // Rebases every row of four of a stage into `out`, which holds the
    // stage's values at N; returns how many rows it rebased.
    size_t rebaseStage(std::span<const float> twoBack, std::span<const float> latest,
                       const Transform3x4& viewAtN, const Transform3x4& viewBetween,
                       std::span<float> out) const;

  private:
    static constexpr size_t kRotationFloats = 12;

    struct HeldRotation {
        std::array<float, kRotationFloats> rows{};
        size_t draws{1};
        size_t lastDraw{0};
    };

    std::vector<HeldRotation> m_rotations;
    size_t m_drawsAdded{0};
    std::vector<Vec3> m_axes;
};

} // namespace wiiuport::interp
