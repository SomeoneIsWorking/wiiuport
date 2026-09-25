#include "vertex_blend_fixture.h"

#include "check.h"

#include <bit>

namespace wiiuport::tests::vertex_blend {

using frame::FrameRecording;
using frame::RecordedUniformAssembly;

void putWord(uint8_t endian, std::byte* at, uint32_t word) {
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

RecordedUniformAssembly assemblyOf(const ActorDraw& draw) {
    RecordedUniformAssembly assembly;
    assembly.shaderBaseHash = kActorShader;
    assembly.blockSources = {1, draw.block};
    assembly.data = draw.uniforms;
    return assembly;
}

LatteFrameHooks::DrawPrepared preparedOf(std::span<const std::byte> mesh, bool fromRuntime,
                                         const ActorDraw& draw) {
    LatteFrameHooks::DrawPrepared prepared{};
    prepared.vertexShaderBaseHash = kActorShader;
    prepared.vertexUniforms = true;
    prepared.fromRuntime = fromRuntime;
    prepared.vertexReplaceable = fromRuntime;
    prepared.vertexBuffers[0] = {mesh.data(), static_cast<uint32_t>(mesh.size()),
                                 static_cast<uint32_t>(draw.valuesPerVertex * sizeof(float)), 0};
    prepared.vertexBufferCount = 1;
    prepared.vertexAttributes[0] = {
        0,    static_cast<uint32_t>(draw.valueFetched * sizeof(float)), 4, kFloat1, kBigEndian, 0,
        false};
    prepared.vertexAttributeCount = 1;
    return prepared;
}

void Blends::record(const GuestFrame& guest) {
    const GuestFrame& frame = recorded.emplace_back(guest);
    FrameRecording recording;
    for (size_t index = 0; index < frame.draws.size(); ++index) {
        if (!frame.draws[index].sharesUniforms) {
            RecordedUniformAssembly assembly = assemblyOf(frame.draws[index]);
            objects.onAssemblyRecorded(assembly);
            vertices.onAssemblyRecorded(assembly);
            recording.addUniformAssembly(assembly);
        }
        vertices.onDrawRecorded(preparedOf(frame.meshOf(index), false, frame.draws[index]));
    }
    objects.onFrameRecorded(recording);
    vertices.onFrameRecorded(recording);
}

std::vector<std::vector<float>> Blends::replay(const GuestFrame& latest) {
    const GuestFrame& frame = recorded.back();
    check::equal(frame.draws.size(), latest.draws.size(), "the frame replayed is the latest");
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
        if (!draw.sharesUniforms) {
            objects.apply(assembly);
        }
        LatteFrameHooks::VertexReplacements replacements;
        vertices.onRuntimeDraw(preparedOf(frame.meshOf(index), true, draw), replacements);
        const auto* bytes = static_cast<const std::byte*>(
            replacements.data[0] != nullptr ? replacements.data[0] : frame.meshOf(index).data());
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

} // namespace wiiuport::tests::vertex_blend
