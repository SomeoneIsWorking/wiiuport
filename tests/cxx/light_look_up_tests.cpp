#include "check.h"
#include "suites.h"
#include "wiiuport/interp/LightLookUp.h"
#include "wiiuport/interp/Transform3x4.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
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

// Rows of four, a rotation with the fourth values `w`, scaled by `scale`.
std::vector<float> rotationRows(std::array<Vec3, 3> axes, std::array<float, 3> w, float scale) {
    std::vector<float> rows;
    for (size_t row = 0; row < 3; ++row) {
        rows.insert(rows.end(),
                    {axes[row].x * scale, axes[row].y * scale, axes[row].z * scale, w[row]});
    }
    return rows;
}

Vec3 axisOf(const Transform3x4& rotation, size_t index) {
    const std::array<float, Transform3x4::kFloats>& l = rotation.values();
    return {l[index * 4], l[(index * 4) + 1], l[(index * 4) + 2]};
}

std::array<Vec3, 3> axesOf(const Transform3x4& rotation) {
    return {axisOf(rotation, 0), axisOf(rotation, 1), axisOf(rotation, 2)};
}

// A camera turning on a level, and the light turning with it.
struct Scene {
    Transform3x4 light = turned(0.4f, false, {0.0f, 0.0f, 0.0f});
    Transform3x4 lightTwoBack = turned(0.3f, false, {0.0f, 0.0f, 0.0f});
    Transform3x4 viewAtN = turned(0.6f, true, {3.0f, -2.0f, 40.0f});
    Transform3x4 viewTwoBack = turned(0.45f, true, {3.5f, -2.0f, 41.0f});
    Transform3x4 viewBetween = turned(0.52f, true, {3.2f, -2.0f, 40.5f});

    // The frame's draws into the map. Three hold the light's rotation, each
    // with an object's own: one that stood still -- the world's own axes --
    // and two that moved but are no rotation of the light's space, one
    // translated and one scaled. Two more, one a cascade, hold a caster
    // turning about the upright. Taken for axes, the decoys would put a row
    // across every light axis in a plane of two, and the caster's upright a
    // scalar seen through a level camera.
    LightLookUp lookUp() const {
        LightLookUp found;
        for (const MapDraw& draw : mapDraws()) {
            found.addMapValues(draw.twoBack, draw.latest);
        }
        found.chooseAxes();
        return found;
    }

    struct MapDraw {
        std::vector<float> twoBack;
        std::vector<float> latest;
    };

    std::vector<MapDraw> mapDraws() const {
        Vec3 a = axis(0);
        Vec3 b = axis(1);
        Vec3 c = axis(2);
        float half = 1.0f / std::sqrt(2.0f);
        std::array<Vec3, 3> decoy = {
            Vec3{(a.x - b.x) * half, (a.y - b.y) * half, (a.z - b.z) * half},
            Vec3{(a.x + b.x) * half, (a.y + b.y) * half, (a.z + b.z) * half}, c};
        std::array<Vec3, 3> world = {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
                                     Vec3{0.0f, 0.0f, 1.0f}};
        std::vector<float> lightRows = rotationRows(axesOf(light), {0.0f, 0.0f, 0.0f}, 1.0f);
        std::vector<float> lightRowsTwoBack =
            rotationRows(axesOf(lightTwoBack), {0.0f, 0.0f, 0.0f}, 1.0f);
        std::array<std::array<std::vector<float>, 2>, 3> own = {{
            {rotationRows(world, {0.0f, 0.0f, 0.0f}, 1.0f),
             rotationRows(world, {0.0f, 0.0f, 0.0f}, 1.0f)},
            {rotationRows(decoy, {6.0f, 0.0f, 0.0f}, 1.0f),
             rotationRows(decoy, {7.0f, 0.0f, 0.0f}, 1.0f)},
            {rotationRows(decoy, {0.0f, 0.0f, 0.0f}, 3.0f),
             rotationRows(decoy, {0.0f, 0.0f, 0.0f}, 2.0f)},
        }};
        std::vector<MapDraw> draws;
        for (const std::array<std::vector<float>, 2>& object : own) {
            MapDraw draw{lightRowsTwoBack, lightRows};
            draw.twoBack.insert(draw.twoBack.end(), object[0].begin(), object[0].end());
            draw.latest.insert(draw.latest.end(), object[1].begin(), object[1].end());
            draws.push_back(draw);
        }
        MapDraw caster{rotationRows(axesOf(turned(0.2f, true, {})), {0.0f, 0.0f, 0.0f}, 1.0f),
                       rotationRows(axesOf(turned(0.5f, true, {})), {0.0f, 0.0f, 0.0f}, 1.0f)};
        draws.push_back(caster);
        draws.push_back(caster);
        return draws;
    }

    Vec3 axis(size_t index) const {
        return axisOf(light, index);
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

void theAxesAreTheRotationTheMapDrawsTurned() {
    Scene scene;
    check::equal(scene.lookUp().axisCount(), size_t{3}, "three axes, the rest refused");
    LightLookUp still;
    std::vector<float> light = rotationRows(axesOf(scene.light), {0.0f, 0.0f, 0.0f}, 1.0f);
    still.addMapValues(light, light);
    still.addMapValues(light, light);
    still.chooseAxes();
    check::equal(still.axisCount(), size_t{0}, "a rotation that stood still is not the light's");
    std::vector<Scene::MapDraw> draws = scene.mapDraws();
    LightLookUp even;
    for (size_t draw = 0; draw < draws.size(); ++draw) {
        // The light's rotation in two draws, as many as the caster's.
        if (draw != 0) {
            even.addMapValues(draws[draw].twoBack, draws[draw].latest);
        }
    }
    even.chooseAxes();
    check::equal(even.axisCount(), size_t{0},
                 "two rotations held by as many draws do not name the light");
    LightLookUp once;
    once.addMapValues(draws[0].twoBack, draws[0].latest);
    once.chooseAxes();
    check::equal(once.axisCount(), size_t{0}, "a rotation one draw holds is an object's");
    LightLookUp twice;
    std::vector<float> doubled = draws.back().latest;
    doubled.insert(doubled.end(), draws.back().latest.begin(), draws.back().latest.end());
    std::vector<float> doubledTwoBack = draws.back().twoBack;
    doubledTwoBack.insert(doubledTwoBack.end(), draws.back().twoBack.begin(),
                          draws.back().twoBack.end());
    twice.addMapValues(doubledTwoBack, doubled);
    twice.chooseAxes();
    check::equal(twice.axisCount(), size_t{0}, "a draw holding a rotation twice is one draw");
    // The translated and the scaled decoys, each in every draw.
    for (size_t decoy : {size_t{1}, size_t{2}}) {
        std::span<const float> latest = std::span<const float>(draws[decoy].latest).subspan(12);
        std::span<const float> twoBack = std::span<const float>(draws[decoy].twoBack).subspan(12);
        LightLookUp alone;
        alone.addMapValues(twoBack, latest);
        alone.addMapValues(twoBack, latest);
        alone.chooseAxes();
        check::equal(alone.axisCount(), size_t{0},
                     "rows translated or scaled are no rotation of the light's");
    }
    Row row{1.0f, 0.0f, 0.0f, 0.0f};
    Row moved{0.0f, 1.0f, 0.0f, 0.0f};
    check::isTrue(still.classify(row, moved, scene.viewAtN) == LightLookUp::RowForm::Unrelated,
                  "without the light's axes, no row is the light's");
}

void aScalarIsNotTurnedThroughTheWorldsOwnAxes() {
    Scene scene;
    LightLookUp lookUp = scene.lookUp();
    Row twoBack{2570.792f, 0.0f, 0.0f, 0.0f};
    Row latest{755.9406f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> out = latest;
    check::equal(lookUp.rebaseStage(twoBack, latest, scene.viewAtN, scene.viewBetween, out),
                 size_t{0}, "a moved scalar seen through a level camera is not the light's");
    check::isTrue(out == latest, "the scalar is drawn as the title drew it");
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
    theAxesAreTheRotationTheMapDrawsTurned();
    aScalarIsNotTurnedThroughTheWorldsOwnAxes();
    aMovedDepthRowIsRebasedToTheInBetweenView();
    aMixedRowOfTwoAxesIsTheLights();
    aDirectionKeepsItsFourthValue();
    aStillRowAndARowOffTheLightsPlanesAreLeft();
}

} // namespace wiiuport::tests
