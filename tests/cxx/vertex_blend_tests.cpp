#include "check.h"
#include "suites.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/interp/ObjectBlend.h"
#include "wiiuport/interp/VertexBlend.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

constexpr float kHalfway = 0.5f;
constexpr PartnerIdentity kByBlocks = PartnerIdentity::ByBlocks;
constexpr uint8_t kFloat3 = 0x30;
constexpr uint8_t kFloat1 = 0x0E;
// 8_8_8_8 UNORM: a colour, not floats.
constexpr uint8_t kColour = 0x1A;
constexpr uint8_t kLittleEndian = 0;
constexpr uint8_t kBigEndian = 2;
// SWAP_U16: halves swapped, which no float lerp reads.
constexpr uint8_t kSwapU16 = 1;
// A vertex: a position of three floats, then a colour.
constexpr uint32_t kStride = 16;

struct Vertex {
    std::array<float, 3> position;
    uint32_t colour;
};

// A word as the guest keeps it: most significant byte first for big endian.
void putWord(std::byte* at, uint32_t word, uint8_t endian) {
    for (uint32_t index = 0; index < 4; ++index) {
        uint32_t shift = endian == kBigEndian ? 24 - (index * 8) : index * 8;
        at[index] = static_cast<std::byte>((word >> shift) & 0xffu);
    }
}

uint32_t getWord(const std::byte* at, uint8_t endian) {
    uint32_t word = 0;
    for (uint32_t index = 0; index < 4; ++index) {
        uint32_t shift = endian == kBigEndian ? 24 - (index * 8) : index * 8;
        word |= static_cast<uint32_t>(at[index]) << shift;
    }
    return word;
}

// Vertices as the guest keeps them, each word in the given order.
std::vector<std::byte> bytesOf(const std::vector<Vertex>& vertices, uint8_t endian) {
    std::vector<std::byte> bytes(vertices.size() * kStride);
    for (size_t index = 0; index < vertices.size(); ++index) {
        std::array<uint32_t, 4> words{std::bit_cast<uint32_t>(vertices[index].position[0]),
                                      std::bit_cast<uint32_t>(vertices[index].position[1]),
                                      std::bit_cast<uint32_t>(vertices[index].position[2]),
                                      vertices[index].colour};
        for (size_t word = 0; word < words.size(); ++word) {
            putWord(bytes.data() + (index * kStride) + (word * 4), words[word], endian);
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

// --- VertexBlend, fed as the runtime feeds it ---

constexpr uint64_t kActorShader = 0xdddd;
// The blocks two actors alternate between, as the title double-buffers them.
constexpr uint32_t kBlockA = 0xf4001000;
constexpr uint32_t kBlockB = 0xf4081000;
constexpr uint32_t kOtherA = 0xf4002000;
constexpr uint32_t kOtherB = 0xf4082000;

// One of the guest's draws: its uniforms, then its vertices, one float each.
struct ActorDraw {
    uint32_t block;
    std::vector<float> uniforms;
    std::vector<float> mesh;
};

// Where a guest's draw keeps its mesh: each frame's vertices in their own
// buffer, as the title rewrites them.
struct GuestFrame {
    std::vector<ActorDraw> draws;
    std::vector<std::vector<std::byte>> meshes;

    explicit GuestFrame(std::vector<ActorDraw> drawn) : draws(std::move(drawn)) {
        for (const ActorDraw& draw : draws) {
            std::vector<std::byte> bytes(draw.mesh.size() * sizeof(float));
            for (size_t index = 0; index < draw.mesh.size(); ++index) {
                putWord(bytes.data() + (index * sizeof(float)),
                        std::bit_cast<uint32_t>(draw.mesh[index]), kBigEndian);
            }
            meshes.push_back(std::move(bytes));
        }
    }
};

RecordedUniformAssembly assemblyOf(const ActorDraw& draw) {
    RecordedUniformAssembly assembly;
    assembly.shaderBaseHash = kActorShader;
    assembly.blockSources = {1, draw.block};
    assembly.data = draw.uniforms;
    return assembly;
}

LatteFrameHooks::DrawPrepared preparedOf(const std::vector<std::byte>& mesh, bool fromRuntime) {
    LatteFrameHooks::DrawPrepared prepared{};
    prepared.vertexShaderBaseHash = kActorShader;
    prepared.vertexUniforms = true;
    prepared.fromRuntime = fromRuntime;
    prepared.vertexReplaceable = fromRuntime;
    prepared.vertexBuffers[0] = {mesh.data(), static_cast<uint32_t>(mesh.size()), sizeof(float), 0};
    prepared.vertexBufferCount = 1;
    prepared.vertexAttributes[0] = {0, 0, 4, kFloat1, kBigEndian, 0, false};
    prepared.vertexAttributeCount = 1;
    return prepared;
}

struct Blends {
    ObjectBlend objects{kHalfway};
    VertexBlend vertices{objects, kHalfway};

    // One guest frame as the recorder hands it over.
    void record(const GuestFrame& frame) {
        FrameRecording recording;
        for (size_t index = 0; index < frame.draws.size(); ++index) {
            RecordedUniformAssembly assembly = assemblyOf(frame.draws[index]);
            objects.onAssemblyRecorded(assembly);
            vertices.onAssemblyRecorded(assembly);
            vertices.onDrawRecorded(preparedOf(frame.meshes[index], false));
            recording.addUniformAssembly(assembly);
        }
        objects.onFrameRecorded(recording);
        vertices.onFrameRecorded(recording);
    }

    // Replays the latest frame as the product does, returning each draw's
    // mesh as drawn: the replacement where one was handed over.
    std::vector<std::vector<float>> replay(const GuestFrame& frame) {
        objects.armOnce();
        std::vector<std::vector<float>> drawn;
        for (size_t index = 0; index < frame.draws.size(); ++index) {
            const ActorDraw& draw = frame.draws[index];
            std::vector<uint32_t> sources{1, draw.block};
            std::vector<float> uniforms = draw.uniforms;
            LatteFrameHooks::UniformAssembly assembly{};
            assembly.shaderBaseHash = kActorShader;
            assembly.data = uniforms.data();
            assembly.sizeInBytes = static_cast<uint32_t>(uniforms.size() * sizeof(float));
            assembly.blockAddresses = sources.data();
            assembly.blockAddressCount = 1;
            assembly.fromRuntime = true;
            objects.apply(assembly);
            LatteFrameHooks::VertexReplacements replacements;
            vertices.onRuntimeDraw(preparedOf(frame.meshes[index], true), replacements);
            const auto* bytes = static_cast<const std::byte*>(replacements.data[0] != nullptr
                                                                  ? replacements.data[0]
                                                                  : frame.meshes[index].data());
            std::vector<float> mesh(draw.mesh.size());
            for (size_t value = 0; value < mesh.size(); ++value) {
                mesh[value] =
                    std::bit_cast<float>(getWord(bytes + (value * sizeof(float)), kBigEndian));
            }
            drawn.push_back(std::move(mesh));
        }
        objects.disarm();
        return drawn;
    }
};

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
    std::vector<wiiuport::interp::ShaderVertexOutcomes> byShader = blends.vertices.drawsByShader();
    check::equal(byShader.size(), size_t{1}, "one vertex shader was replayed");
    check::equal(byShader[0].shaderBaseHash, kActorShader, "the actors'");
    check::equal(byShader[0].draws[static_cast<size_t>(VertexOutcome::Blended)], uint64_t{1},
                 "with its blended draw");
    check::equal(byShader[0].draws[static_cast<size_t>(VertexOutcome::NoPartner)], uint64_t{1},
                 "and the one with no partner");
    check::equal(blends.vertices.replaysDiverged(), uint64_t{0}, "the replay kept in step");
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
    bool replaced = blends.vertices.onRuntimeDraw(preparedOf(latest.meshes[0], true), replacements);
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
    aMovedMeshIsBlendedValueByValueInEitherByteOrder();
    aMeshThatDidNotMoveIsUnchangedAndByteIdentical();
    aMeshWhoseOnlyChangeIsNotFloatsIsNotBlended();
    aByteOrderTheDecoderDoesNotReadIsNotBlended();
    aMoveTooSmallToHalveIsOutside();
    aValueThatIsNotANumberIsTakenFromN();
    anotherMeshAFrameBeforeIsNotBlendedTowards();
    aMeshItsBlocksNameIsBlendedThoughItTurnedBack();
    aMeshBackWhereItStoodTwoFramesAgoIsHeld();
    aWalkingActorsMeshIsDrawnBetweenItsPartnersAndItsOwn();
    anIdlingActorsMeshIsBlendedFromItsDrawAFrameBefore();
    cloudsTheTitleReordersAreBlendedFromTheCloudTheyPassed();
    cloudsReorderedSinceTwoFramesBackAreBlendedFromTheirOwn();
    aMeshTheTitleDrewUnderOtherBlocksAFrameBeforeIsBlendedFromThere();
    aCloudIsToldFromOneBesideItByWhatItKeeps();
    aCloudThatHappensToStandHalfWayIsNotTakenForAnother();
    aCloudNoSiblingPassedIsDrawnAsTheTitleDrewIt();
    nothingIsReplacedBeforeThreeFramesArePlanned();
    aReplayOutOfStepStopsReplacing();
    verticesSwitchedOffAreDrawnAsTheTitleDrewThemAndBlendAgainOnceOn();
}

} // namespace wiiuport::tests
