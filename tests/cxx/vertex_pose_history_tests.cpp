/// Whether two ticks' worth of a node's position bytes exist to be blended.
///
/// This is the falsifier for the vertex-stream blend. The pose search found no transform
/// anywhere that moves -- not in the node, not in its sub-object, not in the binder's block,
/// not in 838,155 assembled uniform buffers -- because the title positions geometry on the CPU
/// and hands GX2 a display list of already-transformed vertices. The pose is vertex bytes. That
/// is usable only if the *same node* presents *comparable* position bytes on two ticks, and
/// that is true for a static mesh and false for a reallocating particle buffer, a changing
/// vertex count, or a streamed object.
///
/// So every test here is about the four answers being told apart: one sample, identical,
/// blendable, and uncomparable. A report that collapsed the last two would claim a blend it
/// cannot do.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/VertexPoseHistory.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace {

using wiiuport::title::DrawAttributeCensus;
using wiiuport::title::ObjectIdentityScope;
using wiiuport::title::VertexPoseHistory;
using Prepared = LatteFrameHooks::DrawPrepared;

std::string field(const std::string& body, const std::string& name) {
    const size_t at = body.find("\"" + name + "\":");
    if (at == std::string::npos) {
        return "";
    }
    const size_t start = at + name.size() + 3;
    size_t end = start;
    while (end < body.size() && body[end] != ',' && body[end] != '}') {
        end++;
    }
    return body.substr(start, end - start);
}

// A mesh draw: one buffer of `stride` per vertex, `vertices` of them, a position at the head of
// the stride in three floats, and the buffer's bytes at `positions` -- twelve bytes per vertex,
// laid out the way the attribute says.
struct Mesh {
    std::vector<float> positions;
    uint32_t stride = 12;
};

Prepared aMeshDraw(const Mesh& mesh) {
    Prepared draw{};
    draw.vertexBufferCount = 1;
    draw.vertexBuffers[0].data = mesh.positions.data();
    draw.vertexBuffers[0].sizeInBytes =
        static_cast<uint32_t>(mesh.positions.size() * sizeof(float));
    draw.vertexBuffers[0].stride = mesh.stride;
    draw.vertexBuffers[0].slot = 0;
    draw.vertexAttributeCount = 1;
    draw.vertexAttributes[0].buffer = 0;
    draw.vertexAttributes[0].offset = 0;
    draw.vertexAttributes[0].sizeInBytes = 12;
    draw.vertexAttributes[0].format = 0x30;
    draw.vertexAttributes[0].endianSwap = 0;
    draw.vertexAttributes[0].semanticId = 0;
    draw.vertexAttributes[0].perInstance = false;
    return draw;
}

Mesh aMeshAt(float x) {
    Mesh mesh;
    for (int vertex = 0; vertex < 4; vertex++) {
        mesh.positions.push_back(x + static_cast<float>(vertex));
        mesh.positions.push_back(2.0f * static_cast<float>(vertex));
        mesh.positions.push_back(-1.0f);
    }
    return mesh;
}

// A census that has settled a position, built by feeding it the same draw the history will see,
// so the history is never asked to guess one.
DrawAttributeCensus& aCensusWithPosition() {
    static ObjectIdentityScope scope;
    static DrawAttributeCensus census(&scope);
    static bool settled = false;
    if (!settled) {
        for (uint32_t object = 1; object <= 3; object++) {
            scope.bind(0x43e00000u + object * 0x1000u);
            census.onDrawRecorded(aMeshDraw(aMeshAt(0.0f)));
        }
        settled = true;
    }
    return census;
}

} // namespace

void wiiuport::tests::runVertexPoseHistoryTests() {
    // **Blendable: the same node, two frames apart, the positions moved.** This is the case the
    // whole class exists to find, and the numbers it reports are the ones a substitution needs
    // and cannot invent: the stride, the vertex count, the byte length, and how far the value
    // moved.
    {
        ObjectIdentityScope scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope, &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        history.onDrawRecorded(aMeshDraw(aMeshAt(0.0f)));
        frame.store(2);
        history.onDrawRecorded(aMeshDraw(aMeshAt(0.5f)));
        const std::string body = history.json();
        check::isTrue(field(body, "blendable") == "1",
                      "one node whose position bytes moved between two frames is blendable: " +
                          body);
        check::isTrue(field(body, "framesApart") == "1" && field(body, "fromFrame") == "1" &&
                          field(body, "toFrame") == "2",
                      "and the two samples are a frame apart, which is what makes the movement "
                      "a pose rather than a coincidence of two reads");
        check::isTrue(field(body, "vertices") == "4" && field(body, "positionBytes") == "48",
                      "with four vertices and 48 bytes of position, laid out at the stride the "
                      "draw declared");
        check::isTrue(field(body, "stride") == "12" && field(body, "componentBytes") == "12",
                      "and the stride and component size, because a substitution with the "
                      "wrong stride writes the wrong bytes rather than none");
        check::isTrue(field(body, "biggestComponentDelta") == "0.5",
                      "and the movement in the attribute's own units, which is a half-unit of "
                      "travel and not a count of differing bytes");
    }

    // **Identical is its own answer.** A node whose position bytes do not change between ticks
    // is a static mesh, and a blend of it would be its own input: the check that a stationary
    // object stays stationary, not evidence that blending does anything.
    {
        ObjectIdentityScope scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope, &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        history.onDrawRecorded(aMeshDraw(aMeshAt(3.0f)));
        frame.store(2);
        history.onDrawRecorded(aMeshDraw(aMeshAt(3.0f)));
        const std::string body = history.json();
        check::isTrue(field(body, "identical") == "1" && field(body, "blendable") == "0",
                      "a node whose positions did not move is identical, and is not counted as "
                      "blendable: " +
                          body);
        check::isTrue(field(body, "differingBytes") == "0",
                      "with every byte the same, so the two answers cannot be confused");
    }

    // **Uncomparable is the answer a "blendable" count would hide.** The same node, two ticks,
    // a different vertex count: there is no correspondence between the two samples' components
    // to interpolate, and a lerp across it is arithmetic on unrelated numbers. That is neither
    // "identical" nor "blendable", and the report keeps the three apart.
    {
        ObjectIdentityScope scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope, &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        history.onDrawRecorded(aMeshDraw(aMeshAt(0.0f)));
        frame.store(2);
        Mesh shorter;
        shorter.positions = {1.0f, 2.0f, 3.0f};
        shorter.stride = 12;
        history.onDrawRecorded(aMeshDraw(shorter));
        const std::string body = history.json();
        check::isTrue(field(body, "uncomparable") == "1" && field(body, "blendable") == "0" &&
                          field(body, "identical") == "0",
                      "a node whose vertex count changed between ticks is uncomparable, and is "
                      "in none of the other two columns: " +
                          body);
        check::isTrue(body.find("\"verdict\":\"uncomparable\"") != std::string::npos,
                      "and the per-node verdict says which, so the tally is not the only place "
                      "the distinction exists");
    }

    // **One sample is not a verdict.** A node seen once has nothing to be compared with, and
    // the report must not let it be counted as identical -- which would read as "checked, found
    // static" when nothing was checked.
    {
        ObjectIdentityScope scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope, &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        history.onDrawRecorded(aMeshDraw(aMeshAt(0.0f)));
        const std::string body = history.json();
        check::isTrue(field(body, "oneSample") == "1" && field(body, "identical") == "0" &&
                          field(body, "blendable") == "0",
                      "one sample is its own column and is in neither of the other two: " + body);
        check::isTrue(field(body, "framesApart") == "0",
                      "and the frames apart is zero rather than a number nobody measured");
    }

    // **The sample schedule is per frame, and this is the test for it.** A node drawn forty
    // times inside one frame is forty views of one instant; comparing two of them would report
    // a static field as a static field for the wrong reason -- which is exactly the fault the
    // node scan had, where "0 moved, delta 0" was indistinguishable from a same-frame
    // schedule until the schedule was made explicit.
    {
        ObjectIdentityScope scope;
        std::atomic<uint64_t> frame{7};
        VertexPoseHistory history(&scope, &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        for (int draw = 0; draw < 40; draw++) {
            history.onDrawRecorded(aMeshDraw(aMeshAt(0.25f * static_cast<float>(draw))));
        }
        const std::string body = history.json();
        check::isTrue(field(body, "oneSample") == "1" && field(body, "drawsSeen") == "40",
                      "forty draws inside one frame take one sample, so nothing is compared with "
                      "itself: " +
                          body);
        check::isTrue(field(body, "schedule") == "\"perFrame\"",
                      "and the report says the schedule was per frame, so a negative cannot be "
                      "read as a fact about the title");
    }

    // A draw with no settled position samples nothing, and says so. A history that guessed one
    // would be a history of numbers it made up.
    {
        ObjectIdentityScope scope;
        std::atomic<uint64_t> frame{1};
        DrawAttributeCensus blind(nullptr);
        VertexPoseHistory history(&scope, &blind, &frame);
        scope.bind(0x43e00000u);
        history.onDrawRecorded(aMeshDraw(aMeshAt(0.0f)));
        const std::string body = history.json();
        check::isTrue(field(body, "drawsWithoutPosition") == "1" &&
                          field(body, "nodesTracked") == "0",
                      "with no position named by the census, nothing is sampled and the draws are "
                      "counted: " +
                          body);
    }

    // A stride that cannot hold the attribute is refused, not walked: a copy that steps by
    // less than it reads overlaps itself and produces numbers that look like a mesh.
    {
        ObjectIdentityScope scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope, &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        Mesh mesh = aMeshAt(0.0f);
        mesh.stride = 8; // shorter than the twelve-byte position
        history.onDrawRecorded(aMeshDraw(mesh));
        const std::string body = history.json();
        check::isTrue(field(body, "drawsWithoutPosition") == "1" &&
                          field(body, "nodesTracked") == "0",
                      "a stride of eight cannot hold a twelve-byte attribute, so the draw is "
                      "refused rather than sampled: " +
                          body);
    }

    // The tracked set is bounded, and the refusals counted.
    {
        ObjectIdentityScope scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope, &aCensusWithPosition(), &frame);
        for (uint32_t object = 1; object <= VertexPoseHistory::kNodes + 3; object++) {
            scope.bind(0x43e00000u + object * 0x100u);
            history.onDrawRecorded(aMeshDraw(aMeshAt(0.0f)));
        }
        const std::string body = history.json();
        check::isTrue(field(body, "nodesTracked") == std::to_string(VertexPoseHistory::kNodes),
                      "the tracked set stops at its stated size");
        check::isTrue(field(body, "nodesRefused") == "3",
                      "and the objects beyond it are refused and counted");
    }
}
