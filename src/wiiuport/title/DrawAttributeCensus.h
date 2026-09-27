#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/title/ObjectIdentityScope.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::title {

// Which of a draw's vertex attributes is the position, named from the title's own attribute
// table rather than from a GX2 constant looked up in a header.
//
// **Why this exists at all.** The pose is not held anywhere as a transform: the node's own
// 2588 bytes, the sub-object's 4096 at `node + 0xa1c`, the binder's 64-byte block, and 809,682
// assembled uniform buffers all hold transforms, and every one of them is static, while the only
// values that move sit at row scales of 4 to 81. The title positions geometry on the CPU each
// frame and hands GX2 a display list of already-transformed vertices, so by draw time the pose
// has been consumed into vertex bytes. The in-between frame is therefore a lerp of two vertex
// sets at the game's own draw -- which is not a smaller version of a transform blend, it is
// the exact one, and it needs no transform to lerp because the game already applied it.
//
// The position attribute is the one thing that must be *known*, and it is known by measurement
// rather than by assumption: a position is three components (or four), it is a float, and it is
// the same attribute in almost every draw. A semantic index and a format byte read out of a
// header would be a guess about a title nobody has disassembled; a count of how often each
// (semantic, format, size, buffer, offset) signature recurs across the title's own draws is a
// measurement, and the belief is stated as a bar over distinct nodes so one odd object cannot
// name it.
//
// The report carries the whole histogram. A belief without it is a claim.
class DrawAttributeCensus final : public frame::DrawRecordedListener {
  public:
    // The node, so "recurs across objects" is a cross-object claim and not "recurs a lot".
    // Null is allowed and reported, and then the bar is over draws alone -- stated, because a
    // bar that silently changes its denominator is a bar nobody can check.
    explicit DrawAttributeCensus(const ObjectIdentityScope* scope) : m_scope(scope) {
    }

    void onDrawRecorded(const LatteFrameHooks::DrawPrepared& draw) override;

    std::string json() const;

    // The believed position attribute, or false when nothing cleared the bar. The caller writes
    // through the game's own draw, so it needs the buffer, the offset and the size -- and a
    // substitution with the wrong stride writes the wrong bytes rather than none.
    struct Position {
        bool known = false;
        // The stride of the layout this position belongs to. Zero means the answer was asked
        // over every stride at once, which is a mixture of layouts and is why a draw should ask
        // about its own.
        uint32_t stride = 0;
        uint32_t semanticId = 0;
        uint32_t format = 0;
        uint32_t sizeInBytes = 0;
        uint32_t buffer = 0;
        uint32_t offset = 0;
        uint32_t perInstance = 0;
    };

    Position position() const;

    // **The position for one stride, and why a single global offset is wrong.**
    //
    // The first version named one position for the whole title: the position-sized signature
    // held by a majority of the tracked objects, with its buffer and offset. The real run then
    // showed why that cannot work. Five objects of eight compared cleanly -- zero differing
    // bytes, every magnitude believable -- at a stride of 32. The other two, at strides of 20
    // and 64, reported components out of range: 18 of 24 and 60 of 36. Bytes that are not a
    // position read as one, and the cause is that the attribute the census named sits at offset
    // 0 *of its own layout*. A title has several vertex layouts and each packs its position
    // differently, so one global offset is right for one layout and nonsense for the rest.
    //
    // So the belief is per stride, and a draw is asked about its own. The stride is a property
    // of the draw, so this is the draw's own correspondence rather than an inference -- the
    // same rule the vertex history uses to pair samples.
    Position positionFor(uint32_t stride) const;

    // Every stride the census has seen a position-sized attribute at, with the position it
    // named for it. A reader can see how many layouts there are, which is the number the one
    // global answer was hiding.
    std::vector<uint32_t> stridesWithPosition() const;

    // How many distinct nodes the census tracked, and how many draws it saw. The denominators.
    uint64_t draws() const {
        return m_draws;
    }

    uint64_t nodesTracked() const;

    // One attribute signature, as seen at one draw.
    struct Signature {
        uint32_t semanticId = 0;
        uint32_t format = 0;
        uint32_t sizeInBytes = 0;
        uint32_t perInstance = 0;
        uint32_t buffer = 0;
        uint32_t offset = 0;
        // The buffers this attribute was found in, and their geometry: a substitution has to
        // copy the whole buffer, so its length and stride are part of what "found" means.
        uint64_t draws = 0;
        uint64_t nodes = 0;
        uint32_t stride = 0;
        uint32_t bufferBytes = 0;
        bool operator<(const Signature& other) const;
    };

    // How many distinct nodes are tracked, and how many of each kind's signatures are seen in a
    // majority of them. A handful is enough to separate "every object has this" from "this one
    // does".
    static constexpr size_t kNodes = 8;
    // A position is three or four components. Twelve or sixteen bytes, and nothing else: the
    // sizes are what a three-component position is, and a bar that accepts any size accepts
    // every attribute.
    static constexpr size_t kPositionBytes = 12;
    static constexpr size_t kPositionBytesPadded = 16;
    // How many signatures the report names, most-recurring first.
    static constexpr size_t kExamples = 12;

  private:
    // Per node, its signatures and how many of its draws have been read. Bounded, and the
    // refusal counted: a census that grew with the scene would be a list of every object the
    // title has ever drawn.
    struct Node {
        uint32_t address = 0;
        uint32_t draws = 0;
        std::vector<Signature> signatures;
    };

    // Distinct non-zero nodes. A draw with no identity published is not an object, and
    // counting the zero address as one would put a phantom in the denominator every signature
    // divides by.
    uint64_t nodesTrackedLocked() const;
    // The one bar: the position attribute is the position-sized signature held by a majority of
    // the tracked nodes. With the lock already held, because `json()` holds it and needs the
    // answer and the report is the same answer.
    Position positionLocked() const;
    // The same, over the objects whose draws had this stride, and only those. A stride of zero
    // asks the question over every object, which is what `position()` does.
    Position positionForLocked(uint32_t stride) const;
    // How many distinct objects were seen at this stride, and the bar for them.
    uint64_t nodesAtStrideLocked(uint32_t stride) const;
    // Every stride seen with a position-sized attribute, with the lock already held.
    std::vector<uint32_t> stridesWithPositionLocked() const;

    const ObjectIdentityScope* m_scope = nullptr;
    std::atomic<uint64_t> m_draws{0};
    std::atomic<uint64_t> m_drawsWithoutAttributes{0};
    std::atomic<uint64_t> m_drawsWithoutIdentity{0};
    std::atomic<uint64_t> m_attributesRead{0};
    std::atomic<uint64_t> m_attributesOutOfRange{0};
    std::atomic<uint64_t> m_nodesRefused{0};
    mutable std::mutex m_mutex;
    std::vector<Node> m_nodes;
};

} // namespace wiiuport::title
