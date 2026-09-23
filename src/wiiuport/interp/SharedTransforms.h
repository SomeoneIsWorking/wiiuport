#pragma once

#include "wiiuport/interp/TransformSearch.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace wiiuport::interp {

// The change the in-between frame's camera makes to a 4x4 of a shader's
// values, learned from the draws of it that were blended, so an object that
// could not be blended is still seen through that camera.
//
// Some shaders take no view of their own: each object is handed its own
// matrix with the camera already in it (model-view-projection). An object of
// one that is drawn at N -- unverified, or new -- then stands where N's camera
// put it, and while the camera turns it jumps against a world drawn from
// between the two: the trees on the island did. SharedValues cannot carry it,
// since no two objects hold the same matrix. But every object that stood still
// had its matrix changed by the same transform, W' = T W, the camera's step,
// whatever the object: so where the blended draws of a shader agree on T for a
// 4x4 at one offset, an object drawn at N is drawn with T times its own
// matrix there, which is the blend it would have had had it stood still, and
// the camera's part of the blend had it moved.
//
// Agreement is what makes it the camera and not an object: a transform the
// sampled blended draws do not mostly share is no one's. Rebuilt every frame;
// the storage is kept between frames. Pure.
class SharedTransforms {
  public:
    // Blended draws of a shader examined for its windows: enough to see that
    // most agree, few enough that a frame's end is not spent inverting.
    static constexpr size_t kSampled = 8;
    // At least this many sampled draws must agree, and more than half of
    // those whose window is a matrix.
    static constexpr size_t kAgreeing = 3;
    // The values one window covers: a 4x4, one row after another.
    static constexpr size_t kWindow = 16;
    // Two transforms agree within this, relative to the larger value or to
    // one: a transform's rotation and translation differ by orders of
    // magnitude, and the draws it is learned from were rounded to floats.
    static constexpr double kAgreement = 1e-3;
    // A pivot this small against the matrix's largest value is no inverse.
    static constexpr double kSingular = 1e-6;

    using Matrix = std::array<double, kWindow>;

    void clear();
    // A blended draw: its values at N and what it is drawn with.
    void addBlended(const ShaderKey& shader, std::span<const float> latest,
                    std::span<const float> blended);
    // Done adding: finds each shader's windows and their transforms.
    void index();

    // Writes the camera's transform of `latest` over each of the shader's
    // windows into `out` (as long as `latest`), leaving a window whose every
    // value `keep` marks untouched. Returns the windows written.
    size_t carry(const ShaderKey& shader, std::span<const float> latest,
                 std::span<const uint8_t> keep, std::span<float> out) const;

    // The windows found for a shader, by float offset; for tests and reports.
    std::vector<uint32_t> windowsOf(const ShaderKey& shader) const;

  private:
    struct Sample {
        std::vector<float> latest;
        std::vector<float> blended;
    };

    struct Window {
        uint32_t offset;
        Matrix change;
    };

    std::map<ShaderKey, std::vector<Sample>> m_samples;
    std::map<ShaderKey, std::vector<Window>> m_windows;
};

} // namespace wiiuport::interp
