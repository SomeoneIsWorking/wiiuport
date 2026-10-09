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
#include "command_stream.h"
#include "suites.h"
#include "wiiuport/title/VertexComponent.h"
#include "wiiuport/title/VertexPoseHistory.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace {

using wiiuport::title::DrawAttributeCensus;
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

// The mesh's floats with each word's bytes reversed, as a big-endian buffer holds them.
Mesh bigEndian(Mesh mesh) {
    for (float& value : mesh.positions) {
        uint32_t word = 0;
        std::memcpy(&word, &value, sizeof(word));
        word =
            (word >> 24) | ((word >> 8) & 0x0000ff00u) | ((word << 8) & 0x00ff0000u) | (word << 24);
        std::memcpy(&value, &word, sizeof(value));
    }
    return mesh;
}

Prepared bigEndianDraw(const Mesh& mesh) {
    Prepared draw = aMeshDraw(mesh);
    draw.vertexAttributes[0].endianSwap = wiiuport::title::VertexComponent::kSwapU32;
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
    static wiiuport::tests::CommandStream scope;
    static DrawAttributeCensus census(&scope.identity());
    static bool settled = false;
    if (!settled) {
        for (uint32_t object = 1; object <= 3; object++) {
            scope.bind(0x43e00000u + object * 0x1000u);
            census.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(0.0f))));
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
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope.identity(), &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        history.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(0.0f))));
        frame.store(2);
        history.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(0.5f))));
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

    // The same movement stored big-endian, as GX2 lays out 32-bit floats: decoded in the fetch's
    // byte order it is half a unit of travel, not the distance between two byte-swapped words.
    {
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope.identity(), &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        Mesh before = bigEndian(aMeshAt(0.0f));
        Mesh after = bigEndian(aMeshAt(0.5f));
        history.onDrawRecorded(scope.at(bigEndianDraw(before)));
        frame.store(2);
        history.onDrawRecorded(scope.at(bigEndianDraw(after)));
        const std::string body = history.json();
        check::isTrue(field(body, "blendable") == "1" &&
                          field(body, "biggestComponentDelta") == "0.5",
                      "a big-endian mesh moving half a unit is blendable by half a unit: " + body);
    }

    // **Identical is its own answer.** A node whose position bytes do not change between ticks
    // is a static mesh, and a blend of it would be its own input: the check that a stationary
    // object stays stationary, not evidence that blending does anything.
    {
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope.identity(), &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        history.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(3.0f))));
        frame.store(2);
        history.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(3.0f))));
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
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope.identity(), &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        Mesh placeholder;
        placeholder.positions = {0.0f, 0.0f, 0.0f};
        placeholder.stride = 32;
        for (int tick = 0; tick < 2; tick++) {
            frame.store(static_cast<uint64_t>(tick) + 1);
            history.onDrawRecorded(scope.at(aMeshDraw(placeholder)));
            history.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(0.5f * static_cast<float>(tick)))));
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
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope.identity(), &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        history.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(0.0f))));
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

    // **Bytes that differ and a position that did not move is its own answer.** This is the
    // bitwise fault the node scan had, at vertex level: one real run reported 13,780 differing
    // bytes beside a largest component delta of 1.19e-07 and called it a blend. That is a
    // difference in the low mantissa bits, not a pose. Kept apart from `identical` -- the bytes
    // really did differ and a reader needs to know that -- and apart from `blendable`.
    {
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope.identity(), &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        history.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(1.0f))));
        frame.store(2);
        Mesh nudged = aMeshAt(1.0f);
        // One part in 10^7 of a unit -- the float epsilon at 1.0 is 1.19e-7, so this is the
        // SMALLEST nudge that changes the bits at all. A tenth of that rounds straight back to
        // 1.0f and the test would have measured identical bytes rather than an unmoved
        // position, which is the opposite case.
        nudged.positions[0] += 1e-7f;
        history.onDrawRecorded(scope.at(aMeshDraw(nudged)));
        const std::string body = history.json();
        check::isTrue(field(body, "valueUnchanged") == "1" && field(body, "blendable") == "0",
                      "a shape whose bytes differ but whose values agree is valueUnchanged, and "
                      "is not a blend: " +
                          body);
        check::isTrue(field(body, "differingBytes") != "0",
                      "with the differing bytes still reported, because they did differ and a "
                      "reader is entitled to that fact");
        check::isTrue(field(body, "positionMotion") != "",
                      "and the threshold that separates the two is a number in the report, so "
                      "the boundary can be argued with rather than guessed at");
    }

    // **The geometry beside a verdict is the geometry of the shape that verdict is about.** One
    // run showed 1040 vertices beside a verdict about a different shape, and 13,780 differing
    // bytes against a total of 12,480 -- only possible if the two numbers came from different
    // shapes. So a node with a moving 4-vertex shape and a still 1000-vertex shape reports the
    // 4-vertex geometry, because that is the shape the verdict came from.
    {
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope.identity(), &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        Mesh small = aMeshAt(0.0f);
        Mesh large = aMeshAt(0.0f);
        large.positions.resize(1000 * 3, 7.0f);
        for (int tick = 0; tick < 2; tick++) {
            frame.store(static_cast<uint64_t>(tick) + 1);
            small.positions[0] = 0.5f * static_cast<float>(tick);
            history.onDrawRecorded(scope.at(aMeshDraw(small)));
            history.onDrawRecorded(scope.at(aMeshDraw(large)));
        }
        const std::string body = history.json();
        check::isTrue(field(body, "vertices") == "4" && field(body, "stride") == "12",
                      "the moving four-vertex shape's geometry, not the still thousand-vertex "
                      "one's: " +
                          body);
        check::isTrue(field(body, "positionBytes") == "48",
                      "and its byte length, so the two cannot come from different shapes");
    }

    // **A magnitude beyond any scene is a signal, not a result.** The first real run of this
    // reported component deltas of 1.06e+38 and called them movement. That is what bytes that
    // are not a position at that offset look like when read as a float -- the attribute was
    // named across objects and a draw with a 152-byte stride packs something else at offset
    // zero. A position does not move by 10^38 between two frames, so the magnitude is counted
    // and the verdict says the magnitude is not believable, rather than a number that looks
    // like evidence being printed as evidence.
    {
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope.identity(), &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        Mesh absurd = aMeshAt(0.0f);
        // A plausible byte pattern that is not a position: large exponents.
        const float wild[3] = {3.0e38f, -2.0e38f, 1.0e38f};
        std::memcpy(absurd.positions.data(), wild, sizeof(wild));
        history.onDrawRecorded(scope.at(aMeshDraw(absurd)));
        frame.store(2);
        Mesh moved = absurd;
        moved.positions[0] = 3.1e38f;
        history.onDrawRecorded(scope.at(aMeshDraw(moved)));
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
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{7};
        VertexPoseHistory history(&scope.identity(), &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        for (int draw = 0; draw < 40; draw++) {
            history.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(0.25f * static_cast<float>(draw)))));
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
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{1};
        DrawAttributeCensus blind(nullptr);
        VertexPoseHistory history(&scope.identity(), &blind, &frame);
        scope.bind(0x43e00000u);
        history.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(0.0f))));
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
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope.identity(), &aCensusWithPosition(), &frame);
        scope.bind(0x43e00000u);
        Mesh mesh = aMeshAt(0.0f);
        mesh.stride = 8; // shorter than the twelve-byte position
        history.onDrawRecorded(scope.at(aMeshDraw(mesh)));
        const std::string body = history.json();
        check::isTrue(field(body, "drawsWithoutPosition") == "1" &&
                          field(body, "nodesTracked") == "0",
                      "a stride of eight cannot hold a twelve-byte attribute, so the draw is "
                      "refused rather than sampled: " +
                          body);
    }

    // **The sample is strided, and the first eight are not an answer.** A negative about the
    // first objects a wind game draws is a negative about a sea, a sky and a particle system.
    // So an object is tracked only when it is the first of its own kObjectStride-th, and the
    // eight tracked are spread across the whole run. The stride is a sampling rule rather than a
    // property of the title, so it is a stated number and the refusals are reported beside the
    // belief rather than beside a claim.
    {
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope.identity(), &aCensusWithPosition(), &frame);
        const uint64_t offered = VertexPoseHistory::kNodes * VertexPoseHistory::kObjectStride + 3;
        for (uint64_t object = 1; object <= offered; object++) {
            scope.bind(0x43e00000u + static_cast<uint32_t>(object) * 0x100u);
            history.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(0.0f))));
        }
        const std::string body = history.json();
        check::isTrue(field(body, "nodesTracked") == std::to_string(VertexPoseHistory::kNodes),
                      "eight objects tracked out of " + std::to_string(offered) +
                          " offered: " + body);
        check::isTrue(field(body, "objectsOffered") == std::to_string(offered),
                      "with every offer counted, so the stride's denominator is visible");
        check::isTrue(field(body, "objectStride") ==
                          std::to_string(VertexPoseHistory::kObjectStride),
                      "and the stride itself, so a reader can see the sample is spread rather "
                      "than first-come");
        check::isTrue(field(body, "objectsRefusedByStride") != "0",
                      "while the objects the stride skipped are refused and counted separately "
                      "from the ones that filled the set");
    }

    // The stride is per ARRIVAL, not per address: a reused address must not be sampled once and
    // then never again, which is what a pointer-based stride would do.
    {
        wiiuport::tests::CommandStream scope;
        std::atomic<uint64_t> frame{1};
        VertexPoseHistory history(&scope.identity(), &aCensusWithPosition(), &frame);
        for (uint64_t round = 0; round < VertexPoseHistory::kObjectStride * 2; round++) {
            scope.bind(0x43e00000u);
            history.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(0.0f))));
            scope.bind(0x43e00100u);
            history.onDrawRecorded(scope.at(aMeshDraw(aMeshAt(0.0f))));
        }
        const std::string body = history.json();
        check::isTrue(field(body, "nodesTracked") == "2",
                      "two addresses offered thousands of times track as two objects, because "
                      "the first of each stride landed on the first arrival of each: " +
                          body);
    }
}
