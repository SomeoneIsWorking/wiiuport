#include "wiiuport/interp/LightLookUp.h"

#include "wiiuport/interp/Blendable.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace wiiuport::interp {
namespace {

constexpr size_t kRow = 4;

using Row = std::array<double, kRow>;

double dot(const Vec3& a, const Vec3& b) {
    return (static_cast<double>(a.x) * b.x) + (static_cast<double>(a.y) * b.y) +
           (static_cast<double>(a.z) * b.z);
}

// A row applied to positions seen through `view`, as the same row applied to
// positions in the world: the row times the view, the view's fourth row
// being (0, 0, 0, 1). A direction takes the rotation alone.
Row throughView(std::span<const float> row, const Transform3x4& view, bool direction) {
    const std::array<float, Transform3x4::kFloats>& v = view.values();
    Row out{};
    for (size_t column = 0; column < kRow; ++column) {
        double sum = 0.0;
        for (size_t k = 0; k < 3; ++k) {
            sum += static_cast<double>(row[k]) * v[(k * kRow) + column];
        }
        out[column] = sum;
    }
    out[3] = direction ? row[3] : out[3] + row[3];
    return out;
}

// The world row seen through `view` again: times the view's inverse.
void intoView(const Row& world, const Transform3x4& view, bool direction, std::span<float> out) {
    std::array<float, Transform3x4::kFloats> inverse = view.rigidInverse().values();
    for (size_t column = 0; column < kRow; ++column) {
        double sum = 0.0;
        for (size_t k = 0; k < 3; ++k) {
            sum += world[k] * inverse[(k * kRow) + column];
        }
        out[column] = static_cast<float>(sum);
    }
    out[3] = direction ? static_cast<float>(world[3]) : static_cast<float>(out[3] + world[3]);
}

Vec3 axisOf(std::span<const float> row) {
    return {row[0], row[1], row[2]};
}

// Three rows of four that are a rotation with no translation: unit rows at
// right angles, each fourth value zero.
bool isRotation(std::span<const float> rows) {
    for (size_t row = 0; row < 3; ++row) {
        Vec3 axis = axisOf(rows.subspan(row * kRow));
        if (rows[(row * kRow) + 3] != 0.0f ||
            std::abs(dot(axis, axis) - 1.0) > LightLookUp::kAxisTolerance) {
            return false;
        }
        for (size_t other = row + 1; other < 3; ++other) {
            if (std::abs(dot(axis, axisOf(rows.subspan(other * kRow)))) >
                LightLookUp::kAxisTolerance) {
                return false;
            }
        }
    }
    return true;
}

bool moved(std::span<const float> twoBack, std::span<const float> latest) {
    for (size_t index = 0; index < latest.size(); ++index) {
        if (!sameBits(twoBack[index], latest[index])) {
            return true;
        }
    }
    return false;
}

} // namespace

void LightLookUp::clearAxes() {
    m_rotations.clear();
    m_drawsAdded = 0;
    m_axes.clear();
}

void LightLookUp::addMapValues(std::span<const float> twoBack, std::span<const float> latest) {
    if (twoBack.size() != latest.size()) {
        return;
    }
    ++m_drawsAdded;
    for (size_t at = 0; at + kRotationFloats <= latest.size(); at += kRow) {
        std::span<const float> rows = latest.subspan(at, kRotationFloats);
        if (!isRotation(rows) || !moved(twoBack.subspan(at, kRotationFloats), rows)) {
            continue;
        }
        auto held = std::ranges::find_if(m_rotations, [&rows](const HeldRotation& rotation) {
            return sameBits(std::span<const float>(rotation.rows), rows);
        });
        if (held == m_rotations.end()) {
            HeldRotation rotation;
            std::ranges::copy(rows, rotation.rows.begin());
            rotation.lastDraw = m_drawsAdded;
            m_rotations.push_back(rotation);
        } else if (held->lastDraw != m_drawsAdded) {
            // Counted once a draw: a draw holding it twice is still one.
            held->lastDraw = m_drawsAdded;
            ++held->draws;
        }
    }
}

void LightLookUp::chooseAxes() {
    m_axes.clear();
    auto most = std::ranges::max_element(m_rotations, {}, &HeldRotation::draws);
    if (most == m_rotations.end() || most->draws < 2 ||
        std::ranges::count(m_rotations, most->draws, &HeldRotation::draws) != 1) {
        return;
    }
    for (size_t row = 0; row < kRotationFloats; row += kRow) {
        m_axes.push_back(axisOf(std::span<const float>(most->rows).subspan(row)));
    }
}

LightLookUp::RowForm LightLookUp::classify(std::span<const float> twoBack,
                                           std::span<const float> latest,
                                           const Transform3x4& viewAtN) const {
    if (!moved(twoBack.first(3), latest.first(3)) || m_axes.empty()) {
        return RowForm::Unrelated;
    }
    Row world = throughView(latest, viewAtN, true);
    Vec3 direction{static_cast<float>(world[0]), static_cast<float>(world[1]),
                   static_cast<float>(world[2])};
    double length = std::sqrt(dot(direction, direction));
    if (!(length > 0.0)) {
        return RowForm::Unrelated;
    }
    bool inAPlane = std::ranges::any_of(m_axes, [&](const Vec3& axis) {
        return std::abs(dot(direction, axis)) / length <= kPlaneTolerance;
    });
    if (!inAPlane) {
        return RowForm::Unrelated;
    }
    return sameBits(twoBack[3], latest[3]) ? RowForm::Direction : RowForm::Affine;
}

void LightLookUp::rebase(RowForm form, std::span<const float> latest, const Transform3x4& viewAtN,
                         const Transform3x4& viewBetween, std::span<float> out) {
    if (form == RowForm::Unrelated) {
        std::copy_n(latest.begin(), kRow, out.begin());
        return;
    }
    bool direction = form == RowForm::Direction;
    intoView(throughView(latest, viewAtN, direction), viewBetween, direction, out);
}

size_t LightLookUp::rebaseStage(std::span<const float> twoBack, std::span<const float> latest,
                                const Transform3x4& viewAtN, const Transform3x4& viewBetween,
                                std::span<float> out) const {
    size_t rebased = 0;
    for (size_t at = 0; at + kRow <= latest.size() && at + kRow <= twoBack.size(); at += kRow) {
        RowForm form = classify(twoBack.subspan(at, kRow), latest.subspan(at, kRow), viewAtN);
        if (form == RowForm::Unrelated) {
            continue;
        }
        rebase(form, latest.subspan(at, kRow), viewAtN, viewBetween, out.subspan(at, kRow));
        ++rebased;
    }
    return rebased;
}

} // namespace wiiuport::interp
