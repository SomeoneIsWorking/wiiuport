#pragma once

// What the vertex blend tests drive VertexBlend with: the title's draws as
// the recorder and the renderer hand them over, the buffers the title keeps
// meshes in, and the frames fed through ObjectBlend and VertexBlend together.

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/guest/BufferWriters.h"
#include "wiiuport/interp/ObjectBlend.h"
#include "wiiuport/interp/VertexBlend.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace wiiuport::tests::vertex_blend {

// Half way between the title's two frames, as the product blends.
inline constexpr float kHalfway = 0.5f;
inline constexpr uint8_t kFloat1 = 0x0E;
inline constexpr uint8_t kBigEndian = 2;

// A word as the guest keeps it: most significant byte first for big endian.
void putWord(uint8_t endian, std::byte* at, uint32_t word);
uint32_t getWord(const std::byte* at, uint8_t endian);

inline constexpr uint64_t kActorShader = 0xdddd;
inline constexpr uint64_t kOtherShader = 0xeeee;
// The blocks two actors alternate between, as the title double-buffers them.
inline constexpr uint32_t kBlockA = 0xf4001000;
inline constexpr uint32_t kBlockB = 0xf4081000;
inline constexpr uint32_t kOtherA = 0xf4002000;
inline constexpr uint32_t kOtherB = 0xf4082000;

// One of the guest's draws: its uniforms, then its vertices, one float each.
struct ActorDraw {
    uint32_t block;
    std::vector<float> uniforms;
    std::vector<float> mesh;
    // The earlier draw of the frame whose buffer it reads, as a second pass
    // over one mesh does; its own `mesh` is then that draw's.
    std::optional<size_t> passOver{};
    // Its mesh's values a vertex, and the one its shader fetches.
    uint32_t valuesPerVertex{1};
    uint32_t valueFetched{0};
    // Drawn with the uniforms the draw before it assembled, as a title draws
    // several meshes of one object: no assembly of its own.
    bool sharesUniforms{false};
    // Where in the buffer it reads its vertices start, in values: a draw
    // over part of another's mesh, as a toon outline reads the hair's.
    uint32_t fromValue{0};
    // The buffer the title keeps its vertices in from frame to frame, by
    // KeptBuffers' index; none, and each frame's are in a buffer of their own.
    std::optional<size_t> keptIn{};
    // Its vertex shader: another reads the same buffer as a shadow volume
    // reads a line's.
    uint64_t shader{kActorShader};
};

// Buffers the title keeps an object's vertices in across frames, at
// addresses that do not move.
struct KeptBuffers {
    std::array<std::vector<std::byte>, 8> buffers;
};

// Where a guest's draw keeps its mesh: each frame's vertices in their own
// buffer, as the title rewrites them, or in one it keeps.
struct GuestFrame {
    std::vector<ActorDraw> draws;
    std::vector<std::vector<std::byte>> meshes;
    KeptBuffers* kept;

    explicit GuestFrame(std::vector<ActorDraw> drawn, KeptBuffers* keptBuffers = nullptr)
        : draws(std::move(drawn)), kept(keptBuffers) {
        for (const ActorDraw& draw : draws) {
            std::vector<std::byte> bytes(draw.mesh.size() * sizeof(float));
            for (size_t index = 0; index < draw.mesh.size(); ++index) {
                putWord(kBigEndian, bytes.data() + (index * sizeof(float)),
                        std::bit_cast<uint32_t>(draw.mesh[index]));
            }
            if (std::optional<size_t> slot = draw.keptIn; slot.has_value()) {
                std::vector<std::byte>& buffer = kept->buffers.at(*slot);
                buffer.resize(bytes.size());
                std::ranges::copy(bytes, buffer.begin());
                bytes.clear();
            }
            meshes.push_back(std::move(bytes));
        }
    }

    // The bytes the draw at `index` reads.
    std::span<const std::byte> meshOf(size_t index) const {
        size_t reads = draws[index].passOver.value_or(index);
        std::optional<size_t> slot = draws[reads].keptIn;
        std::span<const std::byte> buffer =
            slot.has_value() ? std::span<const std::byte>(kept->buffers.at(*slot))
                             : std::span<const std::byte>(meshes[reads]);
        return buffer.subspan(draws[index].fromValue * sizeof(float));
    }
};

frame::RecordedUniformAssembly assemblyOf(const ActorDraw& draw);
LatteFrameHooks::DrawPrepared preparedOf(std::span<const std::byte> mesh, bool fromRuntime,
                                         const ActorDraw& draw);

struct Blends {
    interp::ObjectBlend objects{kHalfway};
    guest::BufferWriters writers;
    interp::VertexBlend vertices{objects, writers, kHalfway};
    // Every frame recorded, alive to the end: a frame's own buffers are
    // then at addresses no later frame's are, as the title's rewritten ones
    // are not at a kept buffer's two frames on. A test's temporary frame,
    // freed, would hand its address to a later one.
    std::deque<GuestFrame> recorded;

    // One guest frame as the recorder hands it over.
    void record(const GuestFrame& guest);

    // Replays the latest frame -- `latest`, as recorded -- as the product
    // does, returning each draw's mesh as drawn: the replacement where one
    // was handed over.
    std::vector<std::vector<float>> replay(const GuestFrame& latest);
};

} // namespace wiiuport::tests::vertex_blend
