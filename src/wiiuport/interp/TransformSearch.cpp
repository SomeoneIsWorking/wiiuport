#include "wiiuport/interp/TransformSearch.h"

#include <algorithm>
#include <cmath>

namespace wiiuport::interp {
namespace {

// The fourth column of each row.
Vec3 translationAt(const float* values) {
    return Vec3{values[3], values[7], values[11]};
}

float distanceBetween(const Vec3& a, const Vec3& b) {
    auto dx = a.x - b.x;
    auto dy = a.y - b.y;
    auto dz = a.z - b.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

bool spanMatches(const float* haystack, size_t width, const float* needle) {
    if (width < static_cast<size_t>(Transform3x4::kFloats)) {
        return false;
    }
    for (size_t offset = 0; offset + Transform3x4::kFloats <= width; ++offset) {
        auto same = true;
        for (int i = 0; i < Transform3x4::kFloats; ++i) {
            if (haystack[offset + static_cast<size_t>(i)] != needle[i]) {
                same = false;
                break;
            }
        }
        if (same) {
            return true;
        }
    }
    return false;
}

} // namespace

TransformSearch::TransformSearch(float rotationTolerance) : m_rotationTolerance(rotationTolerance) {
}

void TransformSearch::narrowTo(ShaderState& state, size_t width) {
    if (state.width != 0 && width >= state.width) {
        return;
    }
    // A later draw with a shorter buffer narrows what can be compared. Slots
    // past the new width are dropped rather than compared against whatever
    // the shorter buffer left behind.
    state.width = width;
    state.variesWithinFrame.resize(width, 0);
    state.variesAcrossFrames.resize(width, 0);
    state.translationStepSum.resize(width, 0.0);
    state.currentFrame.resize(width, 0.0f);
    state.previousFrame.resize(width, 0.0f);
}

void TransformSearch::closeFrame(ShaderState& state) {
    if (state.currentFrame.empty()) {
        return;
    }
    if (state.hasPreviousFrame) {
        for (size_t slot = 0; slot < state.width; ++slot) {
            if (state.currentFrame[slot] != state.previousFrame[slot]) {
                state.variesAcrossFrames[slot] = 1;
            }
        }
        for (size_t offset = 0; offset + Transform3x4::kFloats <= state.width; ++offset) {
            state.translationStepSum[offset] +=
                distanceBetween(translationAt(&state.currentFrame[offset]),
                                translationAt(&state.previousFrame[offset]));
        }
    }
    state.previousFrame = state.currentFrame;
    state.hasPreviousFrame = true;
    state.frames += 1;
}

void TransformSearch::observe(const frame::FrameRecording& recording) {
    // An incomplete frame is missing draws, and a slot that looks constant
    // only because the draw that would have varied it was dropped is a wrong
    // answer rather than a missing one.
    if (!recording.isComplete()) {
        return;
    }
    std::map<ShaderKey, bool> seenThisFrame;
    for (const auto& assembly : recording.uniformAssemblies()) {
        if (assembly.data.empty()) {
            continue;
        }
        ShaderKey key{assembly.shaderBaseHash, assembly.shaderAuxHash, assembly.stageIndex};
        auto& state = m_shaders[key];
        narrowTo(state, assembly.data.size());
        state.draws += 1;
        auto first = !seenThisFrame[key];
        if (first) {
            seenThisFrame[key] = true;
            std::copy_n(assembly.data.begin(), state.width, state.currentFrame.begin());
            continue;
        }
        for (size_t slot = 0; slot < state.width; ++slot) {
            if (assembly.data[slot] != state.currentFrame[slot]) {
                state.variesWithinFrame[slot] = 1;
            }
        }
    }
    for (auto& [key, seen] : seenThisFrame) {
        if (seen) {
            closeFrame(m_shaders[key]);
        }
    }
    m_framesObserved += 1;
}

uint32_t TransformSearch::countShadersSharing(const ShaderKey& exclude, const float* values) const {
    uint32_t count = 1;
    for (const auto& [key, state] : m_shaders) {
        if (key == exclude || state.frames == 0) {
            continue;
        }
        if (spanMatches(state.currentFrame.data(), state.width, values)) {
            count += 1;
        }
    }
    return count;
}

SearchReport TransformSearch::search() const {
    SearchReport report;
    report.framesObserved = m_framesObserved;
    report.shadersTracked = m_shaders.size();
    if (m_framesObserved < kMinimumFrames) {
        return report;
    }
    for (const auto& [key, state] : m_shaders) {
        if (state.frames < kMinimumFrames) {
            continue;
        }
        for (size_t offset = 0; offset + Transform3x4::kFloats <= state.width; ++offset) {
            report.spansExamined += 1;
            auto varyingWithinFrame = false;
            auto changesBetweenFrames = false;
            for (int i = 0; i < Transform3x4::kFloats; ++i) {
                auto slot = offset + static_cast<size_t>(i);
                varyingWithinFrame = varyingWithinFrame || state.variesWithinFrame[slot] != 0;
                changesBetweenFrames = changesBetweenFrames || state.variesAcrossFrames[slot] != 0;
            }
            // No slot may vary between draws, or this is not one transform the
            // whole frame shares. At least one must vary between frames, or it
            // is a baked-in constant. Requiring every slot to vary between
            // frames would be wrong: a camera that pans without turning leaves
            // its rotation elements untouched.
            if (varyingWithinFrame) {
                report.rejectedVaryingWithinFrame += 1;
                continue;
            }
            if (!changesBetweenFrames) {
                report.rejectedNeverChanging += 1;
                continue;
            }
            auto transform = Transform3x4::fromRowMajor(&state.currentFrame[offset]);
            auto error = transform.rotationError();
            if (error > m_rotationTolerance) {
                report.rejectedRotation += 1;
                continue;
            }
            auto steps = state.frames - 1;
            report.candidates.push_back(TransformCandidate{
                key, static_cast<uint32_t>(offset), state.frames,
                countShadersSharing(key, &state.currentFrame[offset]), error,
                steps == 0 ? 0.0f : static_cast<float>(state.translationStepSum[offset] / steps),
                transform});
        }
    }
    for (const auto& candidate : report.candidates) {
        if (candidate.isShared() && candidate.meanTranslationStep > 0.0f) {
            report.sharedAndMoving += 1;
        }
    }
    std::sort(report.candidates.begin(), report.candidates.end(),
              [](const TransformCandidate& a, const TransformCandidate& b) {
                  if (a.shadersSharing != b.shadersSharing) {
                      return a.shadersSharing > b.shadersSharing;
                  }
                  if (a.rotationError != b.rotationError) {
                      return a.rotationError < b.rotationError;
                  }
                  return a.shader < b.shader;
              });
    return report;
}

} // namespace wiiuport::interp
