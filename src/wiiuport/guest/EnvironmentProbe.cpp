#include "wiiuport/guest/EnvironmentProbe.h"

#include "wiiuport/guest/GuestWords.h"

#include <cstring>

namespace wiiuport::guest {

void EnvironmentProbe::install() {
    GuestCallProbes::Register(kFlush, kFlushFirstInstruction, *this);
}

void EnvironmentProbe::OnInstall(GuestCallProbes::Installation installation) {
    if (installation == GuestCallProbes::Installation::Installed) {
        m_writers.noteInstalled();
    }
}

std::optional<interp::GuestObject> EnvironmentProbe::objectOf(std::span<const uint32_t, 32> gpr,
                                                              uint32_t returnAddress) {
    if (returnAddress == kWaveFlushReturn) {
        uint32_t address =
            gpr[kWavePacketRegister] + kWaves + (gpr[kWaveIndexRegister] * kWaveStride);
        const void* wave = GuestCallProbes::GuestBytes(address, kWaveCounter + 4);
        if (wave == nullptr) {
            return std::nullopt;
        }
        return interp::GuestObject{.address = address, .age = guestFloat(wave, kWaveCounter)};
    }
    const void* flip = GuestCallProbes::GuestBytes(gpr[kCloudFlipRegister], 4);
    if (flip == nullptr || guestWord(flip, 0) > 1) {
        return std::nullopt;
    }
    return interp::GuestObject{.address =
                                   gpr[kBufferRegister] - (guestWord(flip, 0) * kCardBufferStride),
                               .age = std::nullopt};
}

void EnvironmentProbe::OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) {
    if (returnAddress != kWaveFlushReturn && returnAddress != kCloudFlushReturn &&
        returnAddress != kOtherCloudFlushReturn) {
        return;
    }
    std::optional<interp::GuestObject> object = objectOf(gpr, returnAddress);
    const void* buffer = GuestCallProbes::GuestBytes(gpr[kBufferRegister], kBufferVertices + 4);
    const void* source =
        buffer == nullptr
            ? nullptr
            : GuestCallProbes::GuestBytes(guestWord(buffer, kBufferVertices) + gpr[kOffsetRegister],
                                          BufferWriters::kLeadingBytes);
    if (!object.has_value() || source == nullptr) {
        m_writers.noteUnreadable();
        return;
    }
    BufferWriters::Leading leading{};
    std::memcpy(leading.data(), source, leading.size());
    m_writers.record(source, *object, leading);
}

} // namespace wiiuport::guest
