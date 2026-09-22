#include "wiiuport/interp/ViewTracker.h"

#include <map>
#include <utility>

namespace wiiuport::interp {
namespace {

ShaderKey keyOf(const frame::RecordedUniformAssembly& assembly) {
    return ShaderKey{assembly.shaderBaseHash, assembly.shaderAuxHash, assembly.stageIndex};
}

// Each shader's first draw of the frame, which is where a value the whole
// frame shares is read from.
std::map<ShaderKey, const frame::RecordedUniformAssembly*>
firstDraws(const frame::FrameRecording& recording) {
    std::map<ShaderKey, const frame::RecordedUniformAssembly*> first;
    for (const auto& assembly : recording.uniformAssemblies()) {
        if (!assembly.data.empty()) {
            first.emplace(keyOf(assembly), &assembly);
        }
    }
    return first;
}

bool holdsAt(const frame::RecordedUniformAssembly& assembly, size_t offset,
             const Transform3x4& value) {
    return offset + Transform3x4::kFloats <= assembly.data.size() &&
           value.findIn(assembly.data.data() + offset, Transform3x4::kFloats) == 0;
}

} // namespace

ViewTracker::ViewTracker(const TransformSearch& search, float rotationTolerance)
    : m_search(search), m_rotationTolerance(rotationTolerance) {
}

std::optional<Transform3x4>
ViewTracker::followCarriers(const frame::FrameRecording& recording) const {
    auto first = firstDraws(recording);
    std::vector<std::pair<Transform3x4, uint32_t>> votes;
    for (const Carrier& carrier : m_carriers) {
        auto found = first.find(carrier.shader);
        if (found == first.end()) {
            continue;
        }
        const auto& data = found->second->data;
        if (carrier.floatOffset + Transform3x4::kFloats > data.size()) {
            continue;
        }
        auto value = Transform3x4::fromRowMajor(data.data() + carrier.floatOffset);
        bool counted = false;
        for (auto& [candidate, count] : votes) {
            if (candidate.findIn(value.values().data(), Transform3x4::kFloats) == 0) {
                ++count;
                counted = true;
                break;
            }
        }
        if (!counted) {
            votes.emplace_back(value, 1);
        }
    }
    const std::pair<Transform3x4, uint32_t>* best = nullptr;
    for (const auto& vote : votes) {
        if (best == nullptr || vote.second > best->second) {
            best = &vote;
        }
    }
    // One carrier agreeing with itself is not a shared value. The view is the
    // one thing every world pass is handed, so fewer than two means it is gone.
    if (best == nullptr || best->second < 2 || best->first.rotationError() > m_rotationTolerance) {
        return std::nullopt;
    }
    return best->first;
}

std::optional<Transform3x4> ViewTracker::reseed() {
    if (m_framesSinceReseed < kReseedInterval) {
        ++m_framesSinceReseed;
        return std::nullopt;
    }
    m_framesSinceReseed = 0;
    ++m_reseedsRun;
    std::vector<ViewSlot> slots = m_search.viewSlots();
    if (slots.empty()) {
        return std::nullopt;
    }
    ++m_reseedsFound;
    // Every slot carries the same value: the search matched them on it.
    return slots.front().after;
}

void ViewTracker::lose() {
    m_carriers.clear();
    m_view.reset();
    ++m_framesLost;
}

void ViewTracker::onFrameRecorded(const frame::FrameRecording& recording) {
    m_pair.reset();
    if (!recording.isComplete()) {
        lose();
        return;
    }
    std::optional<Transform3x4> now;
    bool continued = false;
    if (m_view.has_value()) {
        now = followCarriers(recording);
        continued = now.has_value();
    }
    if (!now.has_value()) {
        now = reseed();
    }
    if (!now.has_value()) {
        lose();
        return;
    }

    // Every shader of this frame that carries the value, at whatever offset
    // it does, and on every one of its draws -- a slot that differs between
    // draws of one frame is an object's transform that happened to match.
    std::map<ShaderKey, size_t> offsets;
    for (const auto& [shader, assembly] : firstDraws(recording)) {
        size_t offset = now->findIn(assembly->data.data(), assembly->data.size());
        if (offset != Transform3x4::kNotFound) {
            offsets.emplace(shader, offset);
        }
    }
    for (const auto& assembly : recording.uniformAssemblies()) {
        auto found = offsets.find(keyOf(assembly));
        if (found != offsets.end() && !holdsAt(assembly, found->second, *now)) {
            offsets.erase(found);
        }
    }
    if (offsets.size() < 2) {
        lose();
        return;
    }

    std::vector<Carrier> carriers;
    carriers.reserve(offsets.size());
    for (const auto& [shader, offset] : offsets) {
        carriers.push_back(Carrier{shader, static_cast<uint32_t>(offset)});
    }
    // Only a view followed from the previous frame has a previous value that
    // is the same view. After a reseed the value before is unknown until the
    // next frame supplies one.
    if (continued) {
        std::vector<ViewSlot> slots;
        slots.reserve(carriers.size());
        for (const Carrier& carrier : carriers) {
            slots.push_back(ViewSlot{carrier.shader, carrier.floatOffset, *m_view, *now});
        }
        m_pair = std::move(slots);
    }
    m_carriers = std::move(carriers);
    m_view = now;
    ++m_framesTracked;
}

} // namespace wiiuport::interp
