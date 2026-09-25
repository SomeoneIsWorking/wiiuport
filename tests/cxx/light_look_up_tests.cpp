#include "check.h"
#include "suites.h"
#include "wiiuport/interp/LightLookUp.h"
#include "wiiuport/interp/Transform3x4.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

using wiiuport::interp::LightLookUp;
using wiiuport::interp::Transform3x4;
using wiiuport::interp::Vec3;

namespace {

using Row = std::array<float, 4>;

// A rotation of `angle` radians about the axis `y` or `z`, then `offset`.
Transform3x4 turned(float angle, bool aboutY, Vec3 offset) {
    float c = std::cos(angle);
    float s = std::sin(angle);
    std::array<float, Transform3x4::kFloats> values{};
    if (aboutY) {
        values = {c, 0.0f, s, offset.x, 0.0f, 1.0f, 0.0f, offset.y, -s, 0.0f, c, offset.z};
    } else {
        values = {c, -s, 0.0f, offset.x, s, c, 0.0f, offset.y, 0.0f, 0.0f, 1.0f, offset.z};
    }
    return Transform3x4::fromRowMajor(values.data());
}

Vec3 applied(const Transform3x4& transform, Vec3 p) {
    const std::array<float, Transform3x4::kFloats>& v = transform.values();
    return {(v[0] * p.x) + (v[1] * p.y) + (v[2] * p.z) + v[3],
            (v[4] * p.x) + (v[5] * p.y) + (v[6] * p.z) + v[7],
            (v[8] * p.x) + (v[9] * p.y) + (v[10] * p.z) + v[11]};
}

double onPoint(const Row& row, Vec3 p, float w) {
    return (static_cast<double>(row[0]) * p.x) + (static_cast<double>(row[1]) * p.y) +
           (static_cast<double>(row[2]) * p.z) + (static_cast<double>(row[3]) * w);
}

// The light's axes, as the draws into its map hold them.
struct Scene {
    Transform3x4 light = turned(0.4f, false, {0.0f, 0.0f, 0.0f});
    Transform3x4 viewAtN = turned(0.6f, true, {3.0f, -2.0f, 40.0f});
    Transform3x4 viewTwoBack = turned(0.45f, true, {3.5f, -2.0f, 41.0f});
    Transform3x4 viewBetween = turned(0.52f, true, {3.2f, -2.0f, 40.5f});

    LightLookUp lookUp() const {
        LightLookUp found;
        const std::array<float, Transform3x4::kFloats>& l = light.values();
        std::vector<float> map = {l[0], l[1], l[2], 0.0f, l[4], l[5], l[6], 0.0f, l[8], l[9], l[10],
                                  0.0f,
                                  // A unit row with a translation, and a scaled row: not
                                  // axes. Taken for one, the first would put a row
                                  // across every axis in a plane of the light's.
                                  (l[0] - l[4]) / std::sqrt(2.0f), (l[1] - l[5]) / std::sqrt(2.0f),
                                  (l[2] - l[6]) / std::sqrt(2.0f), 7.0f, 2.0f, 0.0f, 0.0f, 0.0f};
        found.addMapValues(map);
        return found;
    }

    Vec3 axis(size_t index) const {
        const std::array<float, Transform3x4::kFloats>& l = light.values();
        return {l[index * 4], l[(index * 4) + 1], l[(index * 4) + 2]};
    }
};

// The row a stage holds for the world row (`world`, `w`) seen through `view`:
// one whose value on a point seen through the view is the world row's value
// on the point itself.
Row seenThrough(Vec3 world, float w, const Transform3x4& view, bool direction) {
    Transform3x4 back = view.rigidInverse();
    const std::array<float, Transform3x4::kFloats>& b = back.values();
    Row row{};
    for (size_t column = 0; column < 4; ++column) {
        row[column] = (world.x * b[column]) + (world.y * b[4 + column]) + (world.z * b[8 + column]);
    }
    row[3] = direction ? w : row[3] + w;
    return row;
}

void theAxesAreTheMapsUnitRowsWithoutTranslation() {
    Scene scene;
    check::equal(scene.lookUp().axisCount(), size_t{3}, "three axes, the rest refused");
    LightLookUp none;
    Row row{1.0f, 0.0f, 0.0f, 0.0f};
    Row moved{0.0f, 1.0f, 0.0f, 0.0f};
    check::isTrue(none.classify(row, moved, scene.viewAtN) == LightLookUp::RowForm::Unrelated,
                  "without a map drawn, no row is the light's");
}

void aMovedDepthRowIsRebasedToTheInBetweenView() {
    Scene scene;
    LightLookUp lookUp = scene.lookUp();
    Vec3 depth = scene.axis(2);
    Row latest = seenThrough(depth, 5.0f, scene.viewAtN, false);
    Row twoBack = seenThrough(depth, 5.0f, scene.viewTwoBack, false);
    check::isTrue(lookUp.classify(twoBack, latest, scene.viewAtN) == LightLookUp::RowForm::Affine,
                  "a row along the depth axis whose translation moved is affine");
    std::array<float, 4> out = latest;
    check::equal(lookUp.rebaseStage(twoBack, latest, scene.viewAtN, scene.viewBetween, out),
                 size_t{1}, "the row is rebased");
    for (Vec3 p : {Vec3{1.0f, 2.0f, 3.0f}, Vec3{-20.0f, 4.0f, 60.0f}, Vec3{7.0f, -9.0f, -1.0f}}) {
        double world = (static_cast<double>(depth.x) * p.x) + (static_cast<double>(depth.y) * p.y) +
                       (static_cast<double>(depth.z) * p.z) + 5.0;
        check::isTrue(std::abs(onPoint(out, applied(scene.viewBetween, p), 1.0f) - world) < 1e-3,
                      "seen through the in-between view it looks the map up where the point is");
    }
}

void aMixedRowOfTwoAxesIsTheLights() {
    Scene scene;
    LightLookUp lookUp = scene.lookUp();
    Vec3 a = scene.axis(0);
    Vec3 c = scene.axis(2);
    Vec3 mixed{(0.6f * a.x) + (0.8f * c.x), (0.6f * a.y) + (0.8f * c.y),
               (0.6f * a.z) + (0.8f * c.z)};
    Row latest = seenThrough(mixed, 2.0f, scene.viewAtN, false);
    Row twoBack = seenThrough(mixed, 2.0f, scene.viewTwoBack, false);
    check::isTrue(lookUp.classify(twoBack, latest, scene.viewAtN) == LightLookUp::RowForm::Affine,
                  "a row in the plane of two axes is the light's");
}

void aDirectionKeepsItsFourthValue() {
    Scene scene;
    LightLookUp lookUp = scene.lookUp();
    Vec3 axis = scene.axis(1);
    Row latest = seenThrough(axis, 0.25f, scene.viewAtN, true);
    Row twoBack = seenThrough(axis, 0.25f, scene.viewTwoBack, true);
    check::isTrue(lookUp.classify(twoBack, latest, scene.viewAtN) ==
                      LightLookUp::RowForm::Direction,
                  "a row whose fourth value stood still is a direction");
    std::array<float, 4> out = latest;
    lookUp.rebaseStage(twoBack, latest, scene.viewAtN, scene.viewBetween, out);
    check::isTrue(out[3] == 0.25f, "a direction's fourth value is not moved");
    Vec3 d{0.3f, -0.5f, 0.8f};
    Transform3x4 turnOnly = scene.viewBetween;
    std::array<float, Transform3x4::kFloats> values = turnOnly.values();
    values[3] = values[7] = values[11] = 0.0f;
    Vec3 seen = applied(Transform3x4::fromRowMajor(values.data()), d);
    double world = (static_cast<double>(axis.x) * d.x) + (static_cast<double>(axis.y) * d.y) +
                   (static_cast<double>(axis.z) * d.z);
    check::isTrue(std::abs(onPoint(out, seen, 0.0f) - world) < 1e-4,
                  "a direction is turned to the in-between view");
}

void aStillRowAndARowOffTheLightsPlanesAreLeft() {
    Scene scene;
    LightLookUp lookUp = scene.lookUp();
    Row still = seenThrough(scene.axis(2), 5.0f, scene.viewAtN, false);
    check::isTrue(lookUp.classify(still, still, scene.viewAtN) == LightLookUp::RowForm::Unrelated,
                  "a row that did not move is left: it lies in a plane by chance");
    Vec3 a = scene.axis(0);
    Vec3 b = scene.axis(1);
    Vec3 c = scene.axis(2);
    Vec3 across{(a.x + b.x + c.x) / std::sqrt(3.0f), (a.y + b.y + c.y) / std::sqrt(3.0f),
                (a.z + b.z + c.z) / std::sqrt(3.0f)};
    Row latest = seenThrough(across, 1.0f, scene.viewAtN, false);
    Row twoBack = seenThrough(across, 1.0f, scene.viewTwoBack, false);
    std::array<float, 8> stage{};
    std::array<float, 8> stageTwoBack{};
    std::copy(latest.begin(), latest.end(), stage.begin());
    std::copy(twoBack.begin(), twoBack.end(), stageTwoBack.begin());
    std::copy(still.begin(), still.end(), stage.begin() + 4);
    std::copy(still.begin(), still.end(), stageTwoBack.begin() + 4);
    std::array<float, 8> out = stage;
    check::equal(lookUp.rebaseStage(stageTwoBack, stage, scene.viewAtN, scene.viewBetween, out),
                 size_t{0}, "a row across every axis is not the light's");
    check::isTrue(out == stage, "the stage is drawn as the title drew it");
}

} // namespace

namespace wiiuport::tests {

void runLightLookUpTests() {
    theAxesAreTheMapsUnitRowsWithoutTranslation();
    aMovedDepthRowIsRebasedToTheInBetweenView();
    aMixedRowOfTwoAxesIsTheLights();
    aDirectionKeepsItsFourthValue();
    aStillRowAndARowOffTheLightsPlanesAreLeft();
}

} // namespace wiiuport::tests
