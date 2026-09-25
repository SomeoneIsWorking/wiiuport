#include "check.h"
#include "suites.h"
#include "vertex_blend_fixture.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/interp/ObjectBlend.h"
#include "wiiuport/interp/VertexBlend.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <span>
#include <vector>

using wiiuport::frame::FrameRecording;
using wiiuport::frame::RecordedUniformAssembly;
using wiiuport::interp::blendVertexBytes;
using wiiuport::interp::ObjectBlend;
using wiiuport::interp::PartnerIdentity;
using wiiuport::interp::VertexBlend;
using wiiuport::interp::VertexLayout;
using wiiuport::interp::VertexOutcome;

namespace {

using namespace wiiuport::tests::vertex_blend;

constexpr PartnerIdentity kByBlocks = PartnerIdentity::ByBlocks;
constexpr uint8_t kFloat3 = 0x30;
// 8_8_8_8 UNORM: a colour, not floats.
constexpr uint8_t kColour = 0x1A;
constexpr uint8_t kLittleEndian = 0;
// SWAP_U16: halves swapped, which no float lerp reads.
constexpr uint8_t kSwapU16 = 1;
// A vertex: a position of three floats, then a colour.
constexpr uint32_t kStride = 16;

struct Vertex {
    std::array<float, 3> position;
    uint32_t colour;
};

// Vertices as the guest keeps them, each word in the given order.
std::vector<std::byte> bytesOf(const std::vector<Vertex>& vertices, uint8_t endian) {
    std::vector<std::byte> bytes(vertices.size() * kStride);
    for (size_t index = 0; index < vertices.size(); ++index) {
        std::array<uint32_t, 4> words{std::bit_cast<uint32_t>(vertices[index].position[0]),
                                      std::bit_cast<uint32_t>(vertices[index].position[1]),
                                      std::bit_cast<uint32_t>(vertices[index].position[2]),
                                      vertices[index].colour};
        for (size_t word = 0; word < words.size(); ++word) {
            putWord(endian, bytes.data() + (index * kStride) + (word * 4), words[word]);
        }
    }
    return bytes;
}

std::vector<Vertex> verticesOf(std::span<const std::byte> bytes, uint8_t endian) {
    std::vector<Vertex> vertices(bytes.size() / kStride);
    for (size_t index = 0; index < vertices.size(); ++index) {
        std::array<uint32_t, 4> words{};
        for (size_t word = 0; word < words.size(); ++word) {
            words[word] = getWord(bytes.data() + (index * kStride) + (word * 4), endian);
        }
        vertices[index] = {{std::bit_cast<float>(words[0]), std::bit_cast<float>(words[1]),
                            std::bit_cast<float>(words[2])},
                           words[3]};
    }
    return vertices;
}

VertexLayout layoutOf(uint32_t vertexCount, uint8_t endian) {
    return VertexLayout{{{vertexCount * kStride, kStride}},
                        {{0, 0, 12, kFloat3, endian}, {0, 12, 4, kColour, endian}}};
}

void checkBlendsHalfWay(uint8_t endian, const std::string& order) {
    // Moving steadily: N-1 half way between N-2 and N.
    std::vector<std::byte> twoBack =
        bytesOf({{{-2.0f, 2.0f, -12.0f}, 0x00000000}, {{1.0f, -1.0f, 1.0f}, 0x22222222}}, endian);
    std::vector<std::byte> before =
        bytesOf({{{0.0f, 2.0f, -4.0f}, 0x11111111}, {{1.0f, 1.0f, 1.0f}, 0x22222222}}, endian);
    std::vector<std::byte> after =
        bytesOf({{{2.0f, 2.0f, 4.0f}, 0x33333333}, {{1.0f, 3.0f, 1.0f}, 0x22222222}}, endian);
    std::vector<std::byte> out(after.size());
    VertexOutcome outcome =
        blendVertexBytes(layoutOf(2, endian), twoBack, before, after, kHalfway, kByBlocks, out);
    check::isTrue(outcome == VertexOutcome::Blended, order + ": a moved mesh is blended");
    std::vector<Vertex> blended = verticesOf(out, endian);
    check::equal(blended[0].position[0], 1.0f, order + ": each moved value lies half way");
    check::equal(blended[0].position[1], 2.0f, order + ": one that did not move is kept");
    check::equal(blended[0].position[2], 0.0f, order + ": negative to positive, half way");
    check::equal(blended[1].position[1], 2.0f, order + ": in every vertex, by the stride");
    check::equal(blended[0].colour, uint32_t{0x33333333},
                 order + ": a value that is not a float is taken from N");
}

void aMovedMeshIsBlendedValueByValueInEitherByteOrder() {
    checkBlendsHalfWay(kBigEndian, "big endian");
    checkBlendsHalfWay(kLittleEndian, "little endian");
}

void aMeshBlendedAllTheWayIsDrawnExactlyAsTheTitleDrewIt() {
    // At t=1 what is drawn is frame N byte for byte. a + (b - a) is not b in
    // float for every pair: this one lands 1.4e-6 short, inside the range a
    // blend may take.
    std::vector<std::byte> twoBack = bytesOf({{{211.50984955f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> before = bytesOf({{{105.50984955f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> after = bytesOf({{{-0.48986194f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> out(after.size());
    VertexOutcome outcome =
        blendVertexBytes(layoutOf(1, kBigEndian), twoBack, before, after, 1.0f, kByBlocks, out);
    const std::vector<std::byte>& drawn = outcome == VertexOutcome::Blended ? out : after;
    check::isTrue(drawn == after, "a mesh blended all the way is drawn as the title drew it");
}

void aMeshThatDidNotMoveIsUnchangedAndByteIdentical() {
    std::vector<std::byte> bytes = bytesOf({{{1.0f, 2.0f, 3.0f}, 0x44444444}}, kBigEndian);
    std::vector<std::byte> out(bytes.size());
    VertexOutcome outcome =
        blendVertexBytes(layoutOf(1, kBigEndian), bytes, bytes, bytes, kHalfway, kByBlocks, out);
    check::isTrue(outcome == VertexOutcome::Unchanged, "the same bytes are unchanged");
    check::isTrue(out == bytes, "and drawn byte for byte as N");
}

void aMeshWhoseOnlyChangeIsNotFloatsIsNotBlended() {
    std::vector<std::byte> before = bytesOf({{{1.0f, 2.0f, 3.0f}, 0x44444444}}, kBigEndian);
    std::vector<std::byte> after = bytesOf({{{1.0f, 2.0f, 3.0f}, 0x55555555}}, kBigEndian);
    std::vector<std::byte> out(after.size());
    VertexOutcome outcome =
        blendVertexBytes(layoutOf(1, kBigEndian), before, before, after, kHalfway, kByBlocks, out);
    check::isTrue(outcome == VertexOutcome::NotFloats, "a colour change is no float to blend");
    check::isTrue(out == after, "and N is drawn");
}

void aByteOrderTheDecoderDoesNotReadIsNotBlended() {
    std::vector<std::byte> before = bytesOf({{{1.0f, 2.0f, 3.0f}, 0}}, kLittleEndian);
    std::vector<std::byte> after = bytesOf({{{5.0f, 2.0f, 3.0f}, 0}}, kLittleEndian);
    std::vector<std::byte> out(after.size());
    VertexOutcome outcome =
        blendVertexBytes(layoutOf(1, kSwapU16), before, before, after, kHalfway, kByBlocks, out);
    check::isTrue(outcome == VertexOutcome::NotFloats, "words in another order are not blended");
    check::isTrue(out == after, "and N is drawn");
}

void aMoveTooSmallToHalveIsOutside() {
    float one = 1.0f;
    float next = std::nextafter(one, 2.0f);
    float down = one - (next - one);
    std::vector<std::byte> twoBack = bytesOf({{{down, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> before = bytesOf({{{one, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> after = bytesOf({{{next, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> out(after.size());
    VertexOutcome outcome =
        blendVertexBytes(layoutOf(1, kBigEndian), twoBack, before, after, kHalfway, kByBlocks, out);
    check::isTrue(outcome == VertexOutcome::Outside, "an ulp's move has no value between");
}

void anUlpsMoveStaysAtNWhileTheRestOfTheMeshIsHalved() {
    float one = 1.0f;
    float next = std::nextafter(one, 2.0f);
    float down = one - (next - one);
    std::vector<std::byte> twoBack = bytesOf({{{down, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> before = bytesOf({{{one, 2.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> after = bytesOf({{{next, 4.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> out(after.size());
    VertexOutcome outcome =
        blendVertexBytes(layoutOf(1, kBigEndian), twoBack, before, after, kHalfway, kByBlocks, out);
    check::isTrue(outcome == VertexOutcome::Blended, "a mesh that moved is blended");
    check::isTrue(out == bytesOf({{{next, 3.0f, 0.0f}, 0}}, kBigEndian),
                  "the ulp's move is drawn at N and the rest half way");
}

void aValueThatIsNotANumberIsTakenFromN() {
    float nan = std::nanf("");
    std::vector<std::byte> twoBack = bytesOf({{{4.0f, -2.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> before = bytesOf({{{nan, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> after = bytesOf({{{4.0f, 2.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> out(after.size());
    VertexOutcome outcome =
        blendVertexBytes(layoutOf(1, kBigEndian), twoBack, before, after, kHalfway, kByBlocks, out);
    check::isTrue(outcome == VertexOutcome::Blended, "the numbers beside it are blended");
    std::vector<Vertex> blended = verticesOf(out, kBigEndian);
    check::equal(blended[0].position[0], 4.0f, "a value that was no number is N's");
    check::equal(blended[0].position[1], 1.0f, "while its neighbour lies half way");
}

void anotherMeshAFrameBeforeIsNotBlendedTowards() {
    // Its own mesh stood near 100 and moved on to 102; the draw offered a
    // frame before, known only by its place, is some other mesh, at 501.
    std::vector<std::byte> twoBack = bytesOf({{{100.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> before = bytesOf({{{501.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> after = bytesOf({{{102.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> out(after.size());
    VertexOutcome outcome = blendVertexBytes(layoutOf(1, kBigEndian), twoBack, before, after,
                                             kHalfway, PartnerIdentity::ByPlace, out);
    check::isTrue(outcome == VertexOutcome::Unverified, "a mesh it did not pass through");
    check::isTrue(out == after, "is not blended towards: N is drawn");
}

void anotherObjectsMeshFoundByUniformsIsNotBlendedTowards() {
    // The uniforms of another object stood where the object's passed; its
    // mesh, at 501, is not the one that moved from 100 to 102.
    std::vector<std::byte> twoBack = bytesOf({{{100.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> before = bytesOf({{{501.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> after = bytesOf({{{102.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> out(after.size());
    VertexOutcome outcome = blendVertexBytes(layoutOf(1, kBigEndian), twoBack, before, after,
                                             kHalfway, PartnerIdentity::ByValues, out);
    check::isTrue(outcome == VertexOutcome::Unverified, "a mesh it did not pass through");
    check::isTrue(out == after, "is not blended towards: N is drawn");
}

void aMeshItsBlocksNameSetBackToTheStartOfItsRunIsDrawnAtN() {
    // A band of surf runs up the beach and is set back to where it starts:
    // half way is where it never was.
    std::vector<std::byte> twoBack = bytesOf({{{100.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> before = bytesOf({{{104.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> after = bytesOf({{{0.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> out(after.size());
    VertexOutcome outcome =
        blendVertexBytes(layoutOf(1, kBigEndian), twoBack, before, after, kHalfway, kByBlocks, out);
    check::isTrue(outcome == VertexOutcome::Unverified, "it is not blended");
    check::isTrue(out == after, "N is drawn");
}

void aMeshItsBlocksNameIsBlendedThoughItTurnedBack() {
    // Its own animation turned back: N-1 is not near the middle of N-2..N,
    // but its blocks say whose mesh it is.
    std::vector<std::byte> twoBack = bytesOf({{{100.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> before = bytesOf({{{104.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> after = bytesOf({{{102.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> out(after.size());
    VertexOutcome outcome =
        blendVertexBytes(layoutOf(1, kBigEndian), twoBack, before, after, kHalfway, kByBlocks, out);
    check::isTrue(outcome == VertexOutcome::Blended, "a mesh its blocks name is blended");
    check::equal(verticesOf(out, kBigEndian)[0].position[0], 103.0f, "half way from N-1");
}

void aMeshBackWhereItStoodTwoFramesAgoIsHeld() {
    // N-2 and N agree whatever N-1 holds: flipping, not moving.
    std::vector<std::byte> stood = bytesOf({{{3.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> before = bytesOf({{{9.0f, 0.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> out(stood.size());
    VertexOutcome outcome =
        blendVertexBytes(layoutOf(1, kBigEndian), stood, before, stood, kHalfway, kByBlocks, out);
    check::isTrue(outcome == VertexOutcome::Held, "a mesh back where it stood is held");
    check::isTrue(out == stood, "and drawn as N");
}

void aMeshThatStoodStillUntilNIsDrawnAsTheTitleDrewIt() {
    // Parked out of sight from N-2 to N-1, then placed: no step to check
    // the move against, so no half way between the two places.
    std::vector<std::byte> parked = bytesOf({{{3.0f, -4757.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> placed = bytesOf({{{9.0f, 6028.0f, 0.0f}, 0}}, kBigEndian);
    std::vector<std::byte> out(placed.size());
    VertexOutcome outcome =
        blendVertexBytes(layoutOf(1, kBigEndian), parked, parked, placed, kHalfway, kByBlocks, out);
    check::isTrue(outcome == VertexOutcome::Started, "a mesh that stood still until N started");
    check::isTrue(out == placed, "and is drawn as N");
    // One value that moved already by N-1 is a step under way.
    std::vector<std::byte> setOff = bytesOf({{{3.0f, 600.0f, 0.0f}, 0}}, kBigEndian);
    outcome =
        blendVertexBytes(layoutOf(1, kBigEndian), parked, setOff, placed, kHalfway, kByBlocks, out);
    check::isTrue(outcome == VertexOutcome::Blended, "a mesh already moving is blended");
}

// --- VertexBlend, fed as the runtime feeds it ---
void aLayoutDescribesOnlyTheDrawItWasTakenFrom() {
    std::vector<std::byte> mesh(16);
    ActorDraw pair{kBlockA, {}, {}, std::nullopt, 2, 0};
    LatteFrameHooks::DrawPrepared prepared = preparedOf(mesh, true, pair);
    VertexLayout layout = VertexLayout::of(prepared);
    check::isTrue(layout.describes(prepared), "a layout describes the draw it was taken from");

    LatteFrameHooks::DrawPrepared otherStride = prepared;
    otherStride.vertexBuffers[0].stride = 4;
    check::isTrue(!layout.describes(otherStride), "not one whose buffer has another stride");
    LatteFrameHooks::DrawPrepared otherFetch = prepared;
    otherFetch.vertexAttributes[0].offset = 4;
    check::isTrue(!layout.describes(otherFetch), "nor one fetching another value");
    LatteFrameHooks::DrawPrepared moreAttributes = prepared;
    moreAttributes.vertexAttributes[1] = prepared.vertexAttributes[0];
    moreAttributes.vertexAttributeCount = 2;
    check::isTrue(!layout.describes(moreAttributes), "nor one fetching more");
}

void aWalkingActorsMeshIsDrawnBetweenItsPartnersAndItsOwn() {
    Blends blends;
    blends.objects.setPlanning(true);
    // The walker's mesh is posed afresh every frame; the other actor stands.
    blends.record(GuestFrame(
        {{kBlockA, {0.0f, 7.0f}, {10.0f, 0.0f}}, {kOtherA, {5.0f, 5.0f}, {50.0f, 50.0f}}}));
    // N-1 draws them in the other order: the partner is the planner's, not
    // the draw at the same place.
    blends.record(GuestFrame(
        {{kOtherB, {5.0f, 5.0f}, {50.0f, 50.0f}}, {kBlockB, {1.0f, 7.0f}, {12.0f, 0.0f}}}));
    GuestFrame latest(
        {{kBlockA, {2.0f, 7.0f}, {14.0f, 0.0f}}, {kOtherA, {5.0f, 5.0f}, {50.0f, 50.0f}}});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 13.0f, "the walker's vertex lies half way from its N-1 pose");
    check::equal(drawn[0][1], 0.0f, "and one that did not move is N's");
    check::equal(drawn[1][0], 50.0f, "the actor standing still is drawn as N");
    check::equal(blends.vertices.draws(VertexOutcome::Blended), uint64_t{1},
                 "one draw's vertices were blended");
    check::equal(blends.vertices.draws(VertexOutcome::NoPartner), uint64_t{1},
                 "and the held actor, with no partner, is counted rather than dropped");
    check::equal(blends.vertices.drawsByShader().size(), size_t{0},
                 "shader by shader, the replay is published at the next frame's end");
    blends.record(latest);
    std::vector<wiiuport::interp::ShaderVertexOutcomes> byShader = blends.vertices.drawsByShader();
    check::equal(byShader.size(), size_t{1}, "one vertex shader was replayed");
    check::equal(byShader[0].shaderBaseHash, kActorShader, "the actors'");
    check::equal(byShader[0].draws[static_cast<size_t>(VertexOutcome::Blended)], uint64_t{1},
                 "with its blended draw");
    check::equal(byShader[0].draws[static_cast<size_t>(VertexOutcome::NoPartner)], uint64_t{1},
                 "and the one with no partner");
    check::equal(blends.vertices.replaysDiverged(), uint64_t{0}, "the replay kept in step");
}

void eachMeshOfOneObjectIsBlendedFromItsOwnDrawAFrameBefore() {
    // One object's uniforms draw two meshes, each moving its own way: the
    // second is told from the first only by its place among the object's
    // draws, which must name its own draws a frame and two frames before.
    Blends blends;
    blends.objects.setPlanning(true);

    struct WalkerAt {
        float uniform;
        float body;
        float cape;
    };

    auto walker = [](uint32_t block, WalkerAt at) {
        ActorDraw second{block, {}, {at.cape}};
        second.sharesUniforms = true;
        return GuestFrame({{block, {at.uniform, 7.0f}, {at.body}}, second});
    };
    blends.record(walker(kBlockA, {.uniform = 0.0f, .body = 10.0f, .cape = 100.0f}));
    blends.record(walker(kBlockB, {.uniform = 1.0f, .body = 12.0f, .cape = 120.0f}));
    GuestFrame latest = walker(kBlockA, {.uniform = 2.0f, .body = 14.0f, .cape = 140.0f});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 13.0f, "the object's first mesh is drawn half way");
    check::equal(drawn[1][0], 130.0f, "and its second from its own draw a frame before");
}

void everyPassOverAMeshDrawsItAlike() {
    // The walker's mesh is drawn twice at N, the second pass from uniforms
    // of its own that no frame before drew, so it has no partner. Drawing
    // that pass at N beside the first pass half way tore the mesh: a shadow
    // volume's passes disagreeing drew a shadow on the water that neither
    // of the title's frames had.
    Blends blends;
    blends.objects.setPlanning(true);
    blends.record(GuestFrame({{kBlockA, {0.0f, 7.0f}, {10.0f}}}));
    blends.record(GuestFrame({{kBlockB, {1.0f, 7.0f}, {12.0f}}}));
    GuestFrame latest({{kBlockA, {2.0f, 7.0f}, {14.0f}}, {kOtherA, {9.0f}, {14.0f}, 0}});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 13.0f, "the first pass draws the mesh half way");
    check::equal(drawn[1][0], 13.0f, "and so does the pass with no partner of its own");
    check::equal(blends.vertices.draws(VertexOutcome::Blended), uint64_t{2}, "both blended");
}

void aMeshTwoShadersFetchOtherwiseIsDrawnAlikeByBoth() {
    // One buffer of two values a vertex, the first fetched by the walker's
    // shader and the second by a shadow's, whose uniforms no frame before
    // drew. Keyed by its layout too, the shadow's read was drawn at N
    // beside the walker's half way: a shadow volume torn open darkened the
    // pier.
    Blends blends;
    blends.objects.setPlanning(true);
    blends.record(GuestFrame({{kBlockA, {0.0f, 7.0f}, {10.0f, 100.0f}, {}, 2, 0}}));
    blends.record(GuestFrame({{kBlockB, {1.0f, 7.0f}, {12.0f, 102.0f}, {}, 2, 0}}));
    GuestFrame latest({{kBlockA, {2.0f, 7.0f}, {14.0f, 104.0f}, {}, 2, 0},
                       {kOtherA, {9.0f}, {14.0f, 104.0f}, 0, 2, 1}});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 13.0f, "the walker's shader draws its value half way");
    check::equal(drawn[1][1], 103.0f, "and the shadow's draws the value it fetches half way");
    check::equal(blends.vertices.draws(VertexOutcome::Blended), uint64_t{2}, "both blended");
}

void aMeshWhoseBytesAnotherMeshDrawsAtNIsDrawnAtNToo() {
    // The hair's mesh is blended; its outline reads part of the same buffer
    // under uniforms no frame before drew, so it has no partner and draws at
    // N. Half way beside it, the hair showed the black outline through it.
    Blends blends;
    blends.objects.setPlanning(true);
    blends.record(GuestFrame({{kBlockA, {0.0f, 7.0f}, {10.0f, 100.0f}}}));
    blends.record(GuestFrame({{kBlockB, {1.0f, 7.0f}, {12.0f, 102.0f}}}));
    ActorDraw outline{kOtherA, {9.0f}, {104.0f}, 0};
    outline.fromValue = 1;
    GuestFrame latest({{kBlockA, {2.0f, 7.0f}, {14.0f, 104.0f}}, outline});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 14.0f, "the hair is drawn at N");
    check::equal(drawn[1][0], 104.0f, "as its outline is");
    check::equal(blends.vertices.draws(VertexOutcome::SharesBytes), uint64_t{1},
                 "counted as sharing its bytes");
}

void aMeshWhoseBytesAnotherMeshBlendsIsBlendedWithIt() {
    // The outline has uniforms of its own a frame before, and moves with the
    // hair: both are blended, and draw their shared bytes alike.
    Blends blends;
    blends.objects.setPlanning(true);
    // The title's blocks alternate: the A pair at N and N-2, the B pair at N-1.
    auto frame = [](float at, bool pairA) {
        uint32_t block = pairA ? kBlockA : kBlockB;
        uint32_t other = pairA ? kOtherA : kOtherB;
        ActorDraw outline{other, {at + 50.0f}, {100.0f + (2.0f * at)}, 0};
        outline.fromValue = 1;
        return GuestFrame(
            {{block, {at, 7.0f}, {10.0f + (2.0f * at), 100.0f + (2.0f * at)}}, outline});
    };
    blends.record(frame(0.0f, true));
    blends.record(frame(1.0f, false));
    GuestFrame latest = frame(2.0f, true);
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][1], 103.0f, "the hair's shared value is drawn half way");
    check::equal(drawn[1][0], 103.0f, "and the outline's, the same bytes, alike");
    check::equal(blends.vertices.draws(VertexOutcome::Blended), uint64_t{2}, "both blended");
}

void aMeshAnExcludedShaderReadsIsDrawnAsTheTitleDrewIt() {
    Blends blends;
    blends.objects.setPlanning(true);
    blends.vertices.exclude({kOtherShader, kActorShader});
    blends.record(GuestFrame({{kBlockA, {0.0f, 7.0f}, {10.0f}}}));
    blends.record(GuestFrame({{kBlockB, {1.0f, 7.0f}, {12.0f}}}));
    GuestFrame latest({{kBlockA, {2.0f, 7.0f}, {14.0f}}});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 14.0f, "the excluded shader's mesh is drawn as N");
    check::equal(blends.vertices.draws(VertexOutcome::Excluded), uint64_t{1}, "counted excluded");
    blends.vertices.exclude({});
    GuestFrame next({{kBlockB, {3.0f, 7.0f}, {16.0f}}});
    blends.record(next);
    drawn = blends.replay(next);
    check::equal(drawn[0][0], 15.0f, "and blended again once nothing is excluded");
}

void anIdlingActorsMeshIsBlendedFromItsDrawAFrameBefore() {
    Blends blends;
    blends.objects.setPlanning(true);
    // It walks, which teaches the planner its blocks' pairs, then stops
    // while the title goes on posing its mesh: its uniforms are held.
    blends.record(GuestFrame({{kBlockA, {0.0f, 7.0f}, {10.0f}}}));
    blends.record(GuestFrame({{kBlockB, {1.0f, 7.0f}, {12.0f}}}));
    blends.record(GuestFrame({{kBlockA, {2.0f, 7.0f}, {14.0f}}}));
    blends.record(GuestFrame({{kBlockB, {2.0f, 7.0f}, {18.0f}}}));
    GuestFrame latest({{kBlockA, {2.0f, 7.0f}, {22.0f}}});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(blends.objects.objects(ObjectBlend::Outcome::Held), uint64_t{1},
                 "the idling actor is held in its uniforms");
    check::equal(drawn[0][0], 20.0f, "and its mesh lies half way from its pose a frame before");
    check::equal(blends.vertices.draws(VertexOutcome::Blended), uint64_t{1}, "counted blended");
}

// The block every cloud's draw sources alike, at the address of each
// frame's parity, as the title double-buffers it.
constexpr uint32_t kSkyBlock = 0xf4003000;
constexpr uint32_t kSkyBlockB = 0xf4083000;
// A block of the frame before that no other frame sources.
constexpr uint32_t kOtherBlock = 0xf4005000;

void cloudsTheTitleReordersAreBlendedFromTheCloudTheyPassed() {
    // Two clouds drawn with the same uniforms from the same double-buffered
    // block, told apart only by their place. They drift in their uniforms,
    // which teaches the planner the block's pair, then hold there while
    // their meshes move on; N-1 draws them the other way round, so the
    // first draw's planner partner is the other cloud.
    Blends blends;
    blends.objects.setPlanning(true);
    blends.record(GuestFrame({{kSkyBlock, {1.0f}, {90.0f}}, {kSkyBlock, {1.0f}, {490.0f}}}));
    blends.record(GuestFrame({{kSkyBlockB, {2.0f}, {95.0f}}, {kSkyBlockB, {2.0f}, {495.0f}}}));
    blends.record(GuestFrame({{kSkyBlock, {3.0f}, {100.0f}}, {kSkyBlock, {3.0f}, {500.0f}}}));
    blends.record(GuestFrame({{kSkyBlockB, {3.0f}, {501.0f}}, {kSkyBlockB, {3.0f}, {101.0f}}}));
    GuestFrame latest({{kSkyBlock, {3.0f}, {102.0f}}, {kSkyBlock, {3.0f}, {502.0f}}});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(blends.objects.objects(ObjectBlend::Outcome::Held), uint64_t{2},
                 "both clouds are held in their uniforms");
    check::equal(drawn[0][0], 101.5f, "the first cloud is drawn from where it was, not the second");
    check::equal(drawn[1][0], 501.5f, "and the second from where it was");
    check::equal(blends.vertices.draws(VertexOutcome::Blended), uint64_t{2}, "both blended");
    check::equal(blends.vertices.partnersFoundByVertices(), uint64_t{2},
                 "each partner found by its vertices");
}

void aMeshTheTitleDrewUnderOtherBlocksAFrameBeforeIsBlendedFromThere() {
    // Two meshes drawn with the same uniforms from the same double-buffered
    // block, told apart only by their place. A frame before, the draws that
    // block names hold other meshes far off, and the title drew these two
    // later under a block of their own, as it drew a world-space mesh a
    // frame before at the pier.
    Blends blends;
    blends.objects.setPlanning(true);
    blends.record(GuestFrame({{kSkyBlock, {1.0f}, {90.0f}}, {kSkyBlock, {1.0f}, {490.0f}}}));
    blends.record(GuestFrame({{kSkyBlockB, {2.0f}, {95.0f}}, {kSkyBlockB, {2.0f}, {495.0f}}}));
    blends.record(GuestFrame({{kSkyBlock, {3.0f}, {100.0f}}, {kSkyBlock, {3.0f}, {500.0f}}}));
    blends.record(GuestFrame({{kSkyBlockB, {3.0f}, {900.0f}},
                              {kSkyBlockB, {3.0f}, {1300.0f}},
                              {kOtherBlock, {3.0f}, {101.0f}},
                              {kOtherBlock, {3.0f}, {501.0f}}}));
    GuestFrame latest({{kSkyBlock, {3.0f}, {102.0f}}, {kSkyBlock, {3.0f}, {502.0f}}});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 101.5f,
                 "the first mesh is drawn from its draw under the other block");
    check::equal(drawn[1][0], 501.5f, "and the second");
    check::equal(blends.vertices.draws(VertexOutcome::Blended), uint64_t{2}, "both blended");
}

void cloudsReorderedSinceTwoFramesBackAreBlendedFromTheirOwn() {
    // Three clouds a hundred apart drift one a frame. N draws them the other
    // way round from N-2, so the first draw's N-2 by place is another cloud
    // two hundred away, and half way between them stands the third: were
    // the step taken from there, the first cloud would be drawn between the
    // others.
    Blends blends;
    blends.objects.setPlanning(true);
    auto clouds = [](uint32_t block, float uniform, std::array<float, 3> meshes) {
        return GuestFrame({{block, {uniform}, {meshes[0]}},
                           {block, {uniform}, {meshes[1]}},
                           {block, {uniform}, {meshes[2]}}});
    };
    blends.record(clouds(kSkyBlock, 1.0f, {98.0f, 198.0f, 298.0f}));
    blends.record(clouds(kSkyBlockB, 2.0f, {99.0f, 199.0f, 299.0f}));
    blends.record(clouds(kSkyBlock, 3.0f, {100.0f, 200.0f, 300.0f}));
    blends.record(clouds(kSkyBlockB, 3.0f, {101.0f, 201.0f, 301.0f}));
    GuestFrame latest = clouds(kSkyBlock, 3.0f, {302.0f, 202.0f, 102.0f});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 301.5f, "the first draw's cloud is drawn from where it was");
    check::equal(drawn[1][0], 201.5f, "and the second's");
    check::equal(drawn[2][0], 101.5f, "and the third's");
}

void aCloudIsToldFromOneBesideItByWhatItKeeps() {
    // Two cloud quads a step and a half apart, drifting one a frame, each
    // with its own texture cell that it keeps. Two frames on, the other
    // stands nearer the first's place than the first's own draw did, and
    // only the texture cell tells them apart; N draws them the other way
    // round from N-2.
    Blends blends;
    blends.objects.setPlanning(true);
    auto clouds = [](uint32_t block, float uniform, float first, float second, bool swapped) {
        ActorDraw one{block, {uniform}, {first, 0.25f}};
        ActorDraw other{block, {uniform}, {second, 0.75f}};
        return swapped ? GuestFrame({other, one}) : GuestFrame({one, other});
    };
    blends.record(clouds(kSkyBlock, 1.0f, 98.0f, 99.5f, false));
    blends.record(clouds(kSkyBlockB, 2.0f, 99.0f, 100.5f, false));
    blends.record(clouds(kSkyBlock, 3.0f, 100.0f, 101.5f, false));
    blends.record(clouds(kSkyBlockB, 3.0f, 101.0f, 102.5f, true));
    GuestFrame latest = clouds(kSkyBlock, 3.0f, 102.0f, 103.5f, true);
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[1][0], 101.5f, "the first cloud is drawn from where it was");
    check::equal(drawn[1][1], 0.25f, "in its own texture cell");
    check::equal(drawn[0][0], 103.0f, "and so is the other");
    check::equal(drawn[0][1], 0.75f, "in its own");
}

void aPatchIsNotTakenForTheRowBesideItByAValueThatFlips() {
    // Two patches of a grid, told apart only by place, each with a value
    // that flips with the title's double buffering: the same at N-2 and N,
    // other at N-1, and further from the first patch's own than from the
    // second's. By what moved, the first patch a frame before is its own.
    Blends blends;
    blends.objects.setPlanning(true);
    auto patches = [](uint32_t block, float uniform, float step, float flip, float besideFlip) {
        return GuestFrame(
            {{block, {uniform}, {step, flip}}, {block, {uniform}, {208.0f + step, besideFlip}}});
    };
    blends.record(patches(kSkyBlock, 1.0f, -1.0f, 100.0f, 100.0f));
    blends.record(patches(kSkyBlockB, 2.0f, -1.0f, 100.0f, 100.0f));
    blends.record(patches(kSkyBlock, 3.0f, 0.0f, 100.0f, 100.0f));
    blends.record(patches(kSkyBlockB, 3.0f, 1.0f, -1000.0f, 150.0f));
    GuestFrame latest = patches(kSkyBlock, 3.0f, 2.0f, 100.0f, 100.0f);
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 1.5f, "the patch is drawn from where it was");
    check::equal(drawn[0][1], 100.0f, "with its flipping value as N has it");
    check::equal(drawn[1][0], 209.5f, "and the row beside it from where it was");
}

void aCloudThatHappensToStandHalfWayIsNotTakenForAnother() {
    // The first draw's cloud is new two frames back, so its step is taken
    // from another cloud, 600 away; a third cloud a frame before happens to
    // stand half way. Taking whichever sibling lands would draw the new
    // cloud flying from it; the one it resembles a frame before does not
    // land, so it is drawn as the title drew it.
    Blends blends;
    blends.objects.setPlanning(true);
    auto clouds = [](uint32_t block, float uniform, float first, float second) {
        return GuestFrame({{block, {uniform}, {first}}, {block, {uniform}, {second}}});
    };
    blends.record(clouds(kSkyBlock, 1.0f, -2.0f, 398.0f));
    blends.record(clouds(kSkyBlockB, 2.0f, -1.0f, 399.0f));
    blends.record(clouds(kSkyBlock, 3.0f, 0.0f, 400.0f));
    blends.record(clouds(kSkyBlockB, 3.0f, 700.0f, 1001.0f));
    GuestFrame latest = clouds(kSkyBlock, 3.0f, 1002.0f, 3.0f);
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 1002.0f, "the new cloud is not drawn flying from another");
    check::equal(drawn[1][0], 3.0f, "nor is the other");
}

void aCloudNoSiblingPassedIsDrawnAsTheTitleDrewIt() {
    // A frame before, neither cloud was on the way from N-2 to N.
    Blends blends;
    blends.objects.setPlanning(true);
    blends.record(GuestFrame({{kSkyBlock, {1.0f}, {100.0f}}, {kSkyBlock, {1.0f}, {500.0f}}}));
    blends.record(GuestFrame({{kSkyBlock, {1.0f}, {300.0f}}, {kSkyBlock, {1.0f}, {700.0f}}}));
    GuestFrame latest({{kSkyBlock, {1.0f}, {102.0f}}, {kSkyBlock, {1.0f}, {502.0f}}});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 102.0f, "the first cloud is drawn as N");
    check::equal(drawn[1][0], 502.0f, "and so is the second");
    check::equal(blends.vertices.draws(VertexOutcome::Unverified), uint64_t{2},
                 "both counted unverified");
    check::equal(blends.vertices.partnersFoundByVertices(), uint64_t{0}, "none found");
}

void aRingNewAtNInABufferOfItsOwnIsNotTakenForAnother() {
    // Ripple rings drawn alike, told apart only by their place, each kept by
    // the title in a pair of buffers of its own, as it double-buffers its
    // blocks. A ring that shrank from 30 to 20 is gone at N, and a new one
    // stands at 10: the old ring's two frames lie on a line through it, but
    // the new ring's buffer was not drawn two frames back, so it is new.
    Blends blends;
    blends.objects.setPlanning(true);
    KeptBuffers kept;
    auto rings = [&kept](uint32_t block, float uniform,
                         const std::vector<std::pair<float, size_t>>& drawn) {
        std::vector<ActorDraw> draws;
        draws.reserve(drawn.size());
        for (auto [radius, buffer] : drawn) {
            draws.push_back(
                {.block = block, .uniforms = {uniform}, .mesh = {radius}, .keptIn = buffer});
        }
        return GuestFrame(std::move(draws), &kept);
    };
    blends.record(rings(kSkyBlock, 1.0f, {{98.0f, 0}, {50.0f, 2}}));
    blends.record(rings(kSkyBlockB, 2.0f, {{99.0f, 1}, {40.0f, 3}}));
    blends.record(rings(kSkyBlock, 3.0f, {{100.0f, 0}, {30.0f, 2}}));
    blends.record(rings(kSkyBlockB, 3.0f, {{101.0f, 1}, {20.0f, 3}}));
    GuestFrame latest = rings(kSkyBlock, 3.0f, {{102.0f, 0}, {10.0f, 4}});
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 101.5f, "the ring kept in its buffers is blended from its own");
    check::equal(drawn[1][0], 10.0f, "the new ring is drawn as the title drew it");
    check::equal(blends.vertices.draws(VertexOutcome::NoPartner), uint64_t{1},
                 "and counted with no partner");
}

void aRingKeptInItsBuffersIsNotTakenForOneSharingAFlippedValue() {
    // Two rings kept each in a pair of buffers of its own, their second value
    // flipping with the title's double buffering. A frame before, each holds
    // bit for bit the value the other holds at N; where it moved, each is
    // nearest its own, and each is blended from it.
    Blends blends;
    blends.objects.setPlanning(true);
    KeptBuffers kept;
    auto rings = [&kept](uint32_t block, float uniform, float step, bool odd) {
        float flip = odd ? 7.0f : 5.0f;
        float otherFlip = odd ? 5.0f : 7.0f;
        std::vector<ActorDraw> draws{
            {.block = block, .uniforms = {uniform}, .mesh = {step, flip}, .keptIn = odd ? 1 : 0},
            {.block = block,
             .uniforms = {uniform},
             .mesh = {500.0f + step, otherFlip},
             .keptIn = odd ? 3 : 2}};
        return GuestFrame(std::move(draws), &kept);
    };
    blends.record(rings(kSkyBlock, 1.0f, -2.0f, false));
    blends.record(rings(kSkyBlockB, 2.0f, -1.0f, true));
    blends.record(rings(kSkyBlock, 3.0f, 0.0f, false));
    blends.record(rings(kSkyBlockB, 3.0f, 1.0f, true));
    GuestFrame latest = rings(kSkyBlock, 3.0f, 2.0f, false);
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 1.5f, "the first ring is blended from its own");
    check::equal(drawn[1][0], 501.5f, "and so is the second");
}

void aRingGivenTheBlocksOfOneThatGoesOnElsewhereIsNew() {
    // Two rings, each alone in its blocks and kept in a pair of buffers of
    // its own. At N the first ring is drawn under the second's blocks, the
    // second is gone, and a new ring takes the first's blocks: the first's
    // buffers are read at N by another draw, so the new ring has no frame
    // before, however near the first ring's path it stands.
    Blends blends;
    blends.objects.setPlanning(true);
    KeptBuffers kept;
    auto ring = [](uint32_t block, float uniform, float radius, size_t buffer) {
        return ActorDraw{.block = block, .uniforms = {uniform}, .mesh = {radius}, .keptIn = buffer};
    };
    blends.record(
        GuestFrame({ring(kBlockA, 1.0f, 96.0f, 0), ring(kSkyBlock, 11.0f, 50.0f, 2)}, &kept));
    blends.record(
        GuestFrame({ring(kBlockB, 2.0f, 97.0f, 1), ring(kSkyBlockB, 12.0f, 51.0f, 3)}, &kept));
    blends.record(
        GuestFrame({ring(kBlockA, 3.0f, 98.0f, 0), ring(kSkyBlock, 13.0f, 52.0f, 2)}, &kept));
    blends.record(
        GuestFrame({ring(kBlockB, 4.0f, 99.0f, 1), ring(kSkyBlockB, 14.0f, 53.0f, 3)}, &kept));
    GuestFrame latest({ring(kBlockA, 5.0f, 100.2f, 4), ring(kSkyBlock, 15.0f, 100.0f, 0)}, &kept);
    blends.record(latest);
    std::vector<std::vector<float>> drawn = blends.replay(latest);
    check::equal(drawn[0][0], 100.2f, "the new ring is drawn as the title drew it");
    check::isTrue(blends.vertices.draws(VertexOutcome::NoPartner) >= uint64_t{1},
                  "and counted with no partner");
}

// Ripple rings told apart only by place, each kept in two buffers it
// alternates through: one grows from 50 in buffers 0 and 1, another from 28
// in buffers 2 and 3, and a third far off in 4 and 5 goes on throughout, so
// every frame draws a pool of rings. At N the first has ended and a new ring
// stands at 10 in its buffer 0, whose draw two frames back is the old ring at
// 52; the second ring a frame before stands at 31, half way, as a trail of
// rings lies on a line. Returns each draw of N as drawn.
enum class Rings : uint8_t {
    // The old ring drawn a frame before N.
    OldEndedAtN,
    // The old ring not drawn a frame before N.
    OldEndedAFrameEarly,
    // The second ring first drawn a frame before N, at 31: its draw there
    // went on from nothing.
    SecondBeganAtNMinusOne,
};

std::vector<std::vector<float>> ringsAfterOneEnds(Blends& blends, Rings rings) {
    blends.objects.setPlanning(true);
    KeptBuffers kept;
    auto frame = [&kept](uint32_t block, float uniform,
                         const std::vector<std::pair<float, size_t>>& drawn) {
        std::vector<ActorDraw> draws;
        draws.reserve(drawn.size());
        for (auto [radius, buffer] : drawn) {
            draws.push_back(
                {.block = block, .uniforms = {uniform}, .mesh = {radius}, .keptIn = buffer});
        }
        return GuestFrame(std::move(draws), &kept);
    };
    if (rings == Rings::SecondBeganAtNMinusOne) {
        blends.record(frame(kSkyBlock, 1.0f, {{50.0f, 0}, {500.0f, 4}}));
        blends.record(frame(kSkyBlockB, 2.0f, {{51.0f, 1}, {501.0f, 5}}));
        blends.record(frame(kSkyBlock, 3.0f, {{52.0f, 0}, {502.0f, 4}}));
    } else {
        blends.record(frame(kSkyBlock, 1.0f, {{50.0f, 0}, {28.0f, 2}, {500.0f, 4}}));
        blends.record(frame(kSkyBlockB, 2.0f, {{51.0f, 1}, {29.0f, 3}, {501.0f, 5}}));
        blends.record(frame(kSkyBlock, 3.0f, {{52.0f, 0}, {30.0f, 2}, {502.0f, 4}}));
    }
    if (rings == Rings::OldEndedAFrameEarly) {
        blends.record(frame(kSkyBlockB, 4.0f, {{31.0f, 3}, {503.0f, 5}}));
    } else {
        blends.record(frame(kSkyBlockB, 4.0f, {{53.0f, 1}, {31.0f, 3}, {503.0f, 5}}));
    }
    GuestFrame latest = frame(kSkyBlock, 5.0f, {{10.0f, 0}, {32.0f, 2}, {504.0f, 4}});
    blends.record(latest);
    return blends.replay(latest);
}

void aRingNewInTheBuffersOfOneThatEndedIsNotBlendedFromAnotherOnItsPath() {
    // The ring a frame before at 31 went on from buffer 2, not the new
    // ring's buffer 0, and the old ring went on from buffer 0: another ring,
    // which the new one is not drawn flying from.
    Blends blends;
    std::vector<std::vector<float>> drawn = ringsAfterOneEnds(blends, Rings::OldEndedAtN);
    check::equal(drawn[0][0], 10.0f, "the new ring is drawn as the title drew it");
    check::equal(drawn[1][0], 31.5f, "the other ring is blended from its own");

    // Nothing a frame before went on from buffer 0; the ring at 31 went on
    // from another.
    Blends endedEarlier;
    drawn = ringsAfterOneEnds(endedEarlier, Rings::OldEndedAFrameEarly);
    check::equal(drawn[0][0], 10.0f, "a new ring nothing went on from is drawn as drawn");
    check::equal(endedEarlier.vertices.draws(VertexOutcome::NoPartner), uint64_t{1},
                 "and counted with no partner");
    check::equal(drawn[1][0], 31.5f, "while the other ring is blended from its own");

    // The ring at 31 went on from nothing; the old ring went on from buffer
    // 0, so the ring at 31 is not the one that did.
    Blends secondNew;
    drawn = ringsAfterOneEnds(secondNew, Rings::SecondBeganAtNMinusOne);
    check::equal(drawn[0][0], 10.0f,
                 "a new ring is not blended from a ring that went on from nothing");
}

void nothingIsReplacedBeforeThreeFramesArePlanned() {
    Blends blends;
    blends.objects.setPlanning(true);
    GuestFrame first({{kBlockA, {0.0f, 7.0f}, {10.0f}}});
    GuestFrame second({{kBlockB, {1.0f, 7.0f}, {12.0f}}});
    blends.record(first);
    blends.record(second);
    std::vector<std::vector<float>> drawn = blends.replay(second);
    check::equal(drawn[0][0], 12.0f, "with no plan the title's vertices are drawn");
    check::equal(blends.vertices.draws(VertexOutcome::Blended), uint64_t{0},
                 "and nothing is counted blended");
}

void aReplayOutOfStepStopsReplacing() {
    Blends blends;
    blends.objects.setPlanning(true);
    blends.record(GuestFrame({{kBlockA, {0.0f, 7.0f}, {10.0f}}}));
    blends.record(GuestFrame({{kBlockB, {1.0f, 7.0f}, {12.0f}}}));
    GuestFrame latest({{kBlockA, {2.0f, 7.0f}, {14.0f}}});
    blends.record(latest);
    blends.objects.armOnce();
    // A draw with no assembly before it: the replay is not the recording's.
    LatteFrameHooks::VertexReplacements replacements;
    bool replaced = blends.vertices.onRuntimeDraw(
        preparedOf(latest.meshes[0], true, latest.draws[0]), replacements);
    check::isTrue(!replaced, "a draw out of step is drawn as the title drew it");
    check::isTrue(replacements.data[0] == nullptr, "with nothing handed over");
    check::equal(blends.vertices.replaysDiverged(), uint64_t{1}, "and counted");
}

void verticesSwitchedOffAreDrawnAsTheTitleDrewThemAndBlendAgainOnceOn() {
    Blends blends;
    blends.objects.setPlanning(true);
    blends.record(GuestFrame({{kBlockA, {0.0f, 7.0f}, {10.0f}}}));
    blends.record(GuestFrame({{kBlockB, {1.0f, 7.0f}, {12.0f}}}));
    GuestFrame latest({{kBlockA, {2.0f, 7.0f}, {14.0f}}});
    blends.record(latest);
    blends.vertices.setBlending(false);
    check::equal(blends.replay(latest)[0][0], 14.0f, "switched off, the title's vertices");
    check::equal(blends.vertices.draws(VertexOutcome::Blended), uint64_t{0}, "none blended");
    blends.vertices.setBlending(true);
    check::equal(blends.replay(latest)[0][0], 13.0f,
                 "switched back on, the kept vertices blend in the next replay");
}

} // namespace

namespace wiiuport::tests {

void runVertexBlendTests() {
    aLayoutDescribesOnlyTheDrawItWasTakenFrom();
    aMovedMeshIsBlendedValueByValueInEitherByteOrder();
    aMeshBlendedAllTheWayIsDrawnExactlyAsTheTitleDrewIt();
    aMeshThatDidNotMoveIsUnchangedAndByteIdentical();
    aMeshWhoseOnlyChangeIsNotFloatsIsNotBlended();
    aByteOrderTheDecoderDoesNotReadIsNotBlended();
    aMoveTooSmallToHalveIsOutside();
    anUlpsMoveStaysAtNWhileTheRestOfTheMeshIsHalved();
    aValueThatIsNotANumberIsTakenFromN();
    anotherMeshAFrameBeforeIsNotBlendedTowards();
    anotherObjectsMeshFoundByUniformsIsNotBlendedTowards();
    aMeshItsBlocksNameSetBackToTheStartOfItsRunIsDrawnAtN();
    aMeshItsBlocksNameIsBlendedThoughItTurnedBack();
    aMeshBackWhereItStoodTwoFramesAgoIsHeld();
    aMeshThatStoodStillUntilNIsDrawnAsTheTitleDrewIt();
    aWalkingActorsMeshIsDrawnBetweenItsPartnersAndItsOwn();
    anIdlingActorsMeshIsBlendedFromItsDrawAFrameBefore();
    everyPassOverAMeshDrawsItAlike();
    eachMeshOfOneObjectIsBlendedFromItsOwnDrawAFrameBefore();
    aMeshTwoShadersFetchOtherwiseIsDrawnAlikeByBoth();
    aMeshWhoseBytesAnotherMeshDrawsAtNIsDrawnAtNToo();
    aMeshWhoseBytesAnotherMeshBlendsIsBlendedWithIt();
    aMeshAnExcludedShaderReadsIsDrawnAsTheTitleDrewIt();
    cloudsTheTitleReordersAreBlendedFromTheCloudTheyPassed();
    cloudsReorderedSinceTwoFramesBackAreBlendedFromTheirOwn();
    aMeshTheTitleDrewUnderOtherBlocksAFrameBeforeIsBlendedFromThere();
    aCloudIsToldFromOneBesideItByWhatItKeeps();
    aPatchIsNotTakenForTheRowBesideItByAValueThatFlips();
    aCloudThatHappensToStandHalfWayIsNotTakenForAnother();
    aCloudNoSiblingPassedIsDrawnAsTheTitleDrewIt();
    aRingNewAtNInABufferOfItsOwnIsNotTakenForAnother();
    aRingKeptInItsBuffersIsNotTakenForOneSharingAFlippedValue();
    aRingGivenTheBlocksOfOneThatGoesOnElsewhereIsNew();
    aRingNewInTheBuffersOfOneThatEndedIsNotBlendedFromAnotherOnItsPath();
    nothingIsReplacedBeforeThreeFramesArePlanned();
    aReplayOutOfStepStopsReplacing();
    verticesSwitchedOffAreDrawnAsTheTitleDrewThemAndBlendAgainOnceOn();
}

} // namespace wiiuport::tests
