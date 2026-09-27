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

    // **A node drawn with two shapes in one frame is two shapes, and each pairs with itself.**
    // The first version of this kept the first draw of each frame per node, and the real title
    // came back with six of eight objects "shape changed" -- because a node's first draw each
    // frame is a single-vertex placeholder (36 bytes at a stride of 32, which is one vertex)
    // and its real mesh comes later in the same frame. So the first sample was a placeholder and
    // the second was a mesh. Matching by the draw's own shape is the fix, and this is the test
    // for it: a placeholder and a mesh in each of two frames, with the *mesh* moving.
    {
        ObjectIdentityScope scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope, &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        Mesh placeholder;
        placeholder.positions = {0.0f, 0.0f, 0.0f};
        placeholder.stride = 32;
        for (int tick = 0; tick < 2; tick++) {
            frame.store(static_cast<uint64_t>(tick) + 1);
            history.onDrawRecorded(aMeshDraw(placeholder));
            history.onDrawRecorded(aMeshDraw(aMeshAt(0.5f * static_cast<float>(tick))));
        }
        const std::string body = history.json();
        check::isTrue(field(body, "blendable") == "1",
                      "a node whose mesh moved while its placeholder stood still is blendable: " +
                          body);
        check::isTrue(field(body, "shapes") == "2",
                      "and both shapes are reported, because the placeholder pairing is evidence "
                      "too -- it is the check that a stationary object stays stationary");
        check::isTrue(body.find("\"compared\":true") != std::string::npos,
                      "and the comparison is marked as having happened, which is the field the "
                      "first version of this report could not distinguish from a zero");
    }

    // **Nothing paired is its own answer, and it is not "identical".** A node seen once in one
    // frame has nothing to be compared with, and reporting it as identical would read as
    // "checked, found static" when nothing was checked. The shapes it did sample are listed, so
    // a reader can tell "under-sampled" from "sampled one shape of one".
    {
        ObjectIdentityScope scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope, &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        history.onDrawRecorded(aMeshDraw(aMeshAt(0.0f)));
        const std::string body = history.json();
        check::isTrue(field(body, "unpairedShapes") == "1" && field(body, "identical") == "0" &&
                          field(body, "blendable") == "0",
                      "one sample is its own column and is in neither of the other two: " + body);
        check::isTrue(field(body, "framesApart") == "0",
                      "and the frames apart is zero rather than a number nobody measured");
        check::isTrue(body.find("\"compared\":false") != std::string::npos,
                      "and the same null rather than zero applies here: one sample is not a "
                      "measurement of no movement");
        check::isTrue(body.find("\"sampledShapes\"") != std::string::npos &&
                          field(body, "shapes") == "1",
                      "and the shape it did sample is listed, so the under-sampling is visible "
                      "as under-sampling");
    }

    // **A magnitude beyond any scene is a signal, not a result.** The first real run of this
    // reported component deltas of 1.06e+38 and called them movement. That is what bytes that
    // are not a position at that offset look like when read as a float -- the attribute was
    // named across objects and a draw with a 152-byte stride packs something else at offset
    // zero. A position does not move by 10^38 between two frames, so the magnitude is counted
    // and the verdict says the magnitude is not believable, rather than a number that looks
    // like evidence being printed as evidence.
    {
        ObjectIdentityScope scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope, &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        Mesh absurd = aMeshAt(0.0f);
        // A plausible byte pattern that is not a position: large exponents.
        const float wild[3] = {3.0e38f, -2.0e38f, 1.0e38f};
        std::memcpy(absurd.positions.data(), wild, sizeof(wild));
        history.onDrawRecorded(aMeshDraw(absurd));
        frame.store(2);
        Mesh moved = absurd;
        moved.positions[0] = 3.1e38f;
        history.onDrawRecorded(aMeshDraw(moved));
        const std::string body = history.json();
        check::isTrue(body.find("\"magnitudeBelievable\":false") != std::string::npos,
                      "a component delta of 10^38 is counted and the magnitude is marked "
                      "unbelievable: " +
                          body);
        check::isTrue(field(body, "componentsOutOfRange") != "0",
                      "and the count of them is a number, so a reader can see how much of the "
                      "comparison was unreadable rather than a magnitude that means nothing");
        check::isTrue(field(body, "biggestComponentDelta") == "0",
                      "and the reported magnitude is the largest BELIEVABLE one, which is none "
                      "of them here -- not the 10^38 the first run printed as movement");
        check::isTrue(VertexPoseHistory::kComponentCeiling > 1.0e5f,
                      "and the ceiling is a stated number, generous on purpose: the point is to "
                      "catch 10^38, not to bound a scene");
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
        check::isTrue(field(body, "unpairedShapes") == "1" && field(body, "drawsSeen") == "40",
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
