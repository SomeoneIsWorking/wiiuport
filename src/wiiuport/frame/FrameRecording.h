#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wiiuport::frame {

// One frame's guest draw stream, held by copy.
//
// The copy is not caution. The title reuses its display-list storage: of 175
// distinct indirect-buffer addresses seen in one measured run, 175 were
// referenced again in a later frame. Holding pointers would mean replaying
// whatever the guest had most recently written there, which is a different
// frame's geometry presented as this one's.
//
// The same applies to the assembled uniform buffer, which the renderer builds
// into storage it reuses every draw.
//
// This type is pure: it knows nothing about the command processor, the hooks,
// or what any of the recorded floats mean. It is the thing a replay reads and
// the thing a substitution edits.
//
// Clearing keeps the storage: a frame records thousands of assemblies, and
// allocating and freeing each one's vectors every frame was a fifth of the
// rendering thread's time. A recording reused frame after frame allocates
// only when a frame outgrows every one before it.
struct RecordedDisplayList {
    uint32_t physicalAddress{0};
    std::vector<std::byte> data;
};

struct RecordedUniformAssembly {
    uint64_t shaderBaseHash{0};
    uint64_t shaderAuxHash{0};
    uint32_t stageIndex{0};
    // Whether the draw writes any colour buffer; one that writes depth alone
    // renders a map a later draw looks up (LatteFrameHooks::UniformAssembly).
    bool writesColour{true};
    // The guest addresses of the uniform blocks this draw sourced, as
    // (bufferId, physicalAddress) pairs. This is the engine's own storage for
    // the object, and the only identity a recorded draw carries.
    std::vector<uint32_t> blockSources;
    std::vector<float> data;
};

class FrameRecording {
  public:
    // A frame that grows past this is refused rather than recorded in part: a
    // partial frame replays as a partial image, which is harder to see than a
    // missing one. The measured run needed well under a megabyte a frame.
    static constexpr size_t kDefaultByteBudget = size_t{64} * 1024 * 1024;

    explicit FrameRecording(size_t byteBudget = kDefaultByteBudget);

    // Both return false when the recording is over budget, and record nothing.
    bool addDisplayList(uint32_t physicalAddress, const void* data, size_t sizeInBytes);
    bool addUniformAssembly(const RecordedUniformAssembly& assembly);

    void clear();

    std::span<const RecordedDisplayList> displayLists() const {
        return {m_displayLists.data(), m_displayListCount};
    }

    std::span<const RecordedUniformAssembly> uniformAssemblies() const {
        return {m_uniformAssemblies.data(), m_uniformAssemblyCount};
    }

    size_t byteCount() const {
        return m_byteCount;
    }

    // Nonzero means this frame is incomplete and must not be replayed as if it
    // were the whole thing.
    size_t refusedOverBudget() const {
        return m_refusedOverBudget;
    }

    bool isComplete() const {
        return m_refusedOverBudget == 0;
    }

  private:
    bool reserve(size_t bytes);

    size_t m_byteBudget;
    size_t m_byteCount{0};
    size_t m_refusedOverBudget{0};
    // The first counts of each are this frame's; the rest are storage kept
    // from earlier frames.
    std::vector<RecordedDisplayList> m_displayLists;
    size_t m_displayListCount{0};
    std::vector<RecordedUniformAssembly> m_uniformAssemblies;
    size_t m_uniformAssemblyCount{0};
};

} // namespace wiiuport::frame
