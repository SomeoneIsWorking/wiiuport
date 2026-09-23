#include "wiiuport/interp/SharedTransforms.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace wiiuport::interp {

namespace {

using Matrix = SharedTransforms::Matrix;

Matrix windowAt(std::span<const float> values, size_t offset) {
    Matrix matrix{};
    for (size_t index = 0; index < SharedTransforms::kWindow; ++index) {
        matrix[index] = values[offset + index];
    }
    return matrix;
}

Matrix multiply(const Matrix& left, const Matrix& right) {
    Matrix product{};
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            double sum = 0.0;
            for (size_t k = 0; k < 4; ++k) {
                sum += left[(row * 4) + k] * right[(k * 4) + column];
            }
            product[(row * 4) + column] = sum;
        }
    }
    return product;
}

// Gauss-Jordan with partial pivoting; none when the values are not all
// numbers or the matrix is singular.
std::optional<Matrix> inverse(Matrix matrix) {
    double largest = 0.0;
    for (double value : matrix) {
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
        largest = std::max(largest, std::abs(value));
    }
    if (largest == 0.0) {
        return std::nullopt;
    }
    Matrix result{};
    for (size_t index = 0; index < 4; ++index) {
        result[(index * 4) + index] = 1.0;
    }
    for (size_t column = 0; column < 4; ++column) {
        size_t pivot = column;
        for (size_t row = column + 1; row < 4; ++row) {
            if (std::abs(matrix[(row * 4) + column]) > std::abs(matrix[(pivot * 4) + column])) {
                pivot = row;
            }
        }
        if (std::abs(matrix[(pivot * 4) + column]) < SharedTransforms::kSingular * largest) {
            return std::nullopt;
        }
        for (size_t k = 0; k < 4; ++k) {
            std::swap(matrix[(column * 4) + k], matrix[(pivot * 4) + k]);
            std::swap(result[(column * 4) + k], result[(pivot * 4) + k]);
        }
        double scale = matrix[(column * 4) + column];
        for (size_t k = 0; k < 4; ++k) {
            matrix[(column * 4) + k] /= scale;
            result[(column * 4) + k] /= scale;
        }
        for (size_t row = 0; row < 4; ++row) {
            if (row == column) {
                continue;
            }
            double factor = matrix[(row * 4) + column];
            for (size_t k = 0; k < 4; ++k) {
                matrix[(row * 4) + k] -= factor * matrix[(column * 4) + k];
                result[(row * 4) + k] -= factor * result[(column * 4) + k];
            }
        }
    }
    return result;
}

bool agree(const Matrix& one, const Matrix& other) {
    for (size_t index = 0; index < SharedTransforms::kWindow; ++index) {
        double scale = std::max({1.0, std::abs(one[index]), std::abs(other[index])});
        if (std::abs(one[index] - other[index]) > SharedTransforms::kAgreement * scale) {
            return false;
        }
    }
    return true;
}

bool isIdentity(const Matrix& matrix) {
    Matrix identity{};
    for (size_t index = 0; index < 4; ++index) {
        identity[(index * 4) + index] = 1.0;
    }
    return agree(matrix, identity);
}

// The transform most of `changes` share, with how many share it; none when
// fewer than kAgreeing do, or not more than half.
std::optional<std::pair<Matrix, size_t>> agreedChange(const std::vector<Matrix>& changes) {
    size_t best = 0;
    size_t bestAgreeing = 0;
    for (size_t candidate = 0; candidate < changes.size(); ++candidate) {
        auto agreeing = static_cast<size_t>(
            std::count_if(changes.begin(), changes.end(), [&](const Matrix& change) {
                return agree(change, changes[candidate]);
            }));
        if (agreeing > bestAgreeing) {
            best = candidate;
            bestAgreeing = agreeing;
        }
    }
    if (bestAgreeing < SharedTransforms::kAgreeing || bestAgreeing * 2 <= changes.size()) {
        return std::nullopt;
    }
    return std::pair{changes[best], bestAgreeing};
}

} // namespace

void SharedTransforms::clear() {
    m_samples.clear();
    m_windows.clear();
}

void SharedTransforms::addBlended(const ShaderKey& shader, std::span<const float> latest,
                                  std::span<const float> blended) {
    std::vector<Sample>& samples = m_samples[shader];
    if (samples.size() >= kSampled || latest.size() != blended.size()) {
        return;
    }
    samples.push_back(Sample{{latest.begin(), latest.end()}, {blended.begin(), blended.end()}});
}

void SharedTransforms::index() {
    for (const auto& [shader, samples] : m_samples) {
        if (samples.size() < kAgreeing) {
            continue;
        }
        size_t floats = samples.front().latest.size();
        for (const Sample& sample : samples) {
            floats = std::min(floats, sample.latest.size());
        }
        // Every window that is a transform, then the most agreed-on of those
        // that overlap: a 4x4 read a row out of place can agree too.
        std::vector<std::pair<size_t, Window>> found;
        for (size_t offset = 0; offset + kWindow <= floats; offset += 4) {
            std::vector<Matrix> changes;
            for (const Sample& sample : samples) {
                std::optional<Matrix> undone = inverse(windowAt(sample.latest, offset));
                if (undone.has_value()) {
                    changes.push_back(multiply(windowAt(sample.blended, offset), *undone));
                }
            }
            std::optional<std::pair<Matrix, size_t>> change = agreedChange(changes);
            if (change.has_value() && !isIdentity(change->first)) {
                found.emplace_back(change->second,
                                   Window{static_cast<uint32_t>(offset), change->first});
            }
        }
        std::stable_sort(found.begin(), found.end(), [](const auto& one, const auto& other) {
            return one.first > other.first;
        });
        std::vector<Window>& windows = m_windows[shader];
        for (const auto& [agreeing, window] : found) {
            bool overlaps = std::any_of(windows.begin(), windows.end(), [&](const Window& taken) {
                return window.offset < taken.offset + kWindow &&
                       taken.offset < window.offset + kWindow;
            });
            if (!overlaps) {
                windows.push_back(window);
            }
        }
    }
}

size_t SharedTransforms::carry(const ShaderKey& shader, std::span<const float> latest,
                               std::span<const uint8_t> keep, std::span<float> out) const {
    auto found = m_windows.find(shader);
    if (found == m_windows.end()) {
        return 0;
    }
    size_t carried = 0;
    for (const Window& window : found->second) {
        if (window.offset + kWindow > latest.size()) {
            continue;
        }
        auto kept = keep.subspan(window.offset, kWindow);
        if (std::all_of(kept.begin(), kept.end(), [](uint8_t flag) {
                return flag != 0;
            })) {
            continue;
        }
        Matrix moved = multiply(window.change, windowAt(latest, window.offset));
        for (size_t index = 0; index < kWindow; ++index) {
            out[window.offset + index] = static_cast<float>(moved[index]);
        }
        ++carried;
    }
    return carried;
}

std::vector<uint32_t> SharedTransforms::windowsOf(const ShaderKey& shader) const {
    std::vector<uint32_t> offsets;
    auto found = m_windows.find(shader);
    if (found != m_windows.end()) {
        for (const Window& window : found->second) {
            offsets.push_back(window.offset);
        }
    }
    return offsets;
}

} // namespace wiiuport::interp
