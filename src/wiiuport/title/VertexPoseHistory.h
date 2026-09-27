#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/title/DrawAttributeCensus.h"
#include "wiiuport/title/ObjectIdentityScope.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::title {

// Whether two ticks' worth of a node's position bytes exist to be blended, measured before
// anything blends them.
//
// **This is the falsifier for the vertex-stream blend, and it exists because the pose search
// already produced a surprise.** Four places were read -- the node's own 2588 bytes, the
// sub-object's 4096 at `node + 0xa1c`, the binder's 64-byte block, and 838,155 assembled uniform
// buffers -- and every transform in every one of them is static, while the only values that move
// sit at row scales of 4 to 81. The title positions geometry on the CPU each frame and hands
// GX2 a display list of already-transformed vertices, so at draw time there is no pose left to
// lerp: the pose is vertex bytes. That is a better answer than a transform, because it is exact
// rather than an approximation of a rigid map -- but it is only usable if the *same node*
// presents *comparable* position bytes on two different ticks.
//
// **And that is not something to assume.** It is true for a static mesh and false for a
// particle system whose buffer is reallocated, false for a draw whose vertex count changes
// between ticks, and false for a streamed object that is not resident. So this records two
// samples of a bounded set of nodes, one per frame, and reports for each: the stride, the vertex
// count, the byte length, how many bytes differ between the two, and the largest component
// difference in the position attribute's own units. A node whose two samples differ in shape
// rather than in value is counted as **uncomparable**, which is a different answer from
// "identical" and from "blendable", and the report keeps them apart.
//
// The position attribute is not guessed here. It comes from `DrawAttributeCensus`, which named
// it from the title's own attribute tables, and its `endianSwap` is carried into the report
// because a byte-wise comparison is order-independent and a float lerp is not: a substitution
// written against the wrong byte order produces a frame of plausible nonsense.
class VertexPoseHistory final : public frame::DrawRecordedListener {
  public:
    // The frame counter, so a node is sampled once per frame rather than once per binding.
    // Four samples taken inside one frame are four samples of one instant, and the first
    // version of the node scan had exactly that fault: "0 moved, delta 0" was
    // indistinguishable from a static field until the schedule was made explicit.
    VertexPoseHistory(const ObjectIdentityScope* scope, const DrawAttributeCensus* attributes,
                      const std::atomic<uint64_t>* frames);

    // The frame counter, once the paint path exists to give one. Separate from the constructor
    // because the paint mod is a member declared after this one, and a constructor argument
    // would be a member that does not exist yet.
    void setFrameCounter(const std::atomic<uint64_t>* frames) {
        m_frames = frames;
    }

    void onDrawRecorded(const LatteFrameHooks::DrawPrepared& draw) override;

    std::string json() const;

    // How many nodes are in a state where a blend is possible, and how many are in each other
    // state. The denominators, and the answer to "can this work at all".
    struct Tally {
        uint64_t tracked = 0;
        uint64_t blendable = 0;
        uint64_t identical = 0;
        uint64_t unpairedShapes = 0;
        uint64_t refused = 0;
    };

    Tally tally() const;

    // How many distinct nodes are tracked, and how many of their draws have been sampled.
    uint64_t drawsSeen() const {
        return m_drawsSeen;
    }

    // The most bytes one node's position bytes may occupy. A title's largest mesh bounds it,
    // and a node above this is counted and not copied: a bound that a real draw exceeds is a
    // bug in the bound, and silently allocating for it is how a host runs out of memory in a
    // frame.
    static constexpr uint32_t kMaxPositionBytes = 1u << 20;
    // **How far a position component may move between two frames before the bytes are not
    // believed to be a position.** A scene is metres; a view matrix is metres. The first run of
    // this reported component deltas of 1.06e+38 and called them movement -- which is what
    // bytes that are not a float at that offset look like when read as one. The attribute was
    // named across objects and this draw's stride packs something else at offset zero, so the
    // magnitude is a signal that the layout differs and not a measurement. Generous on purpose:
    // the point is to catch 10^38, not to bound a scene.
    static constexpr float kComponentCeiling = 1.0e6f;
    // How many distinct nodes are tracked, how many *shapes* each keeps a sample of, and how
    // many samples each shape keeps. A node is drawn several times a frame -- a placeholder, a
    // depth pass, a colour pass -- and a bound on shapes is what stops one node's whole
    // pipeline from filling the history.
    static constexpr size_t kNodes = 8;
    static constexpr size_t kShapesPerNode = 4;
    static constexpr size_t kSamplesPerShape = 2;

  private:
    // One node's samples.
    //
    // **Keyed by the draw's own shape, and that is the fix for a schedule that measured
    // nothing.** The first version kept the first draw of each frame per node, and the run came
    // back with six of eight objects "uncomparable" for a reason the report then made visible:
    // their first sample was 36 bytes at a stride of 32, which is ONE vertex. A node's first
    // draw each frame is a single-vertex placeholder and its real mesh comes later in the same
    // frame, so sampling the first compared a placeholder with a mesh and called the difference
    // a shape change. A sample is now matched to the previous sample of the SAME shape -- same
    // stride, same length -- so a node's placeholder is compared with the next frame's
    // placeholder and its mesh with the next frame's mesh. The shape is a property of the draw,
    // so this is the draw's own correspondence and not an inference about which draw is "the"
    // one.
    struct Node {
        uint32_t address = 0;

        // Each sample: the frame it came from and the position bytes as the draw laid them out.
        struct Sample {
            uint64_t frame = 0;
            uint32_t stride = 0;
            std::vector<uint8_t> bytes;
        };

        std::vector<Sample> samples;
        // The draw's own numbers, so a shape change is a stated fact rather than a mismatch
        // somebody has to notice.
        uint32_t stride = 0;
        uint32_t componentBytes = 0;
        uint32_t offset = 0;
    };

    // The outcome for one matched pair of samples of one shape.
    enum class Verdict : uint8_t {
        // Only one sample of this shape, so nothing to compare it with.
        OneSample,
        // Two samples of one shape and not one byte differs: a static mesh.
        Identical,
        // Two samples of one shape and the value moved: a pose.
        Blendable,
        // Every shape of this node has a single sample, so nothing was compared. A node that
        // draws a different mesh on each tick lands here rather than in a verdict about two
        // shapes, because there is no comparison to have a verdict about.
        UnpairedShapes
    };

    static Verdict verdictOf(const Node::Sample& before, const Node::Sample& after,
                             uint32_t componentBytes, uint64_t* differingBytes, float* biggestDelta,
                             uint64_t* outOfRange);
    // The same over every paired shape of one node, which is the unit the report and the tally
    // use: a node is as blendable as its best-matched shape, and the report says how many
    // shapes were paired, because a node with one paired shape out of four has not been shown
    // to be static -- it has been shown to be under-sampled.
    Verdict verdictOf(const Node& node, uint64_t* differingBytes, float* biggestDelta,
                      uint64_t* outOfRange) const;
    static const char* nameOf(Verdict verdict);

    const ObjectIdentityScope* m_scope = nullptr;
    const DrawAttributeCensus* m_attributes = nullptr;
    const std::atomic<uint64_t>* m_frames = nullptr;
    std::atomic<uint64_t> m_drawsSeen{0};
    std::atomic<uint64_t> m_drawsWithoutPosition{0};
    std::atomic<uint64_t> m_drawsOversize{0};
    std::atomic<uint64_t> m_drawsWithoutNode{0};
    mutable std::mutex m_mutex;
    std::vector<Node> m_nodes;
    uint64_t m_refused = 0;
};

} // namespace wiiuport::title
