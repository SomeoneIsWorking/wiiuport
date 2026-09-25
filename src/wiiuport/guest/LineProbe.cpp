#include "wiiuport/guest/LineProbe.h"

#include "wiiuport/guest/GuestWords.h"

#include <cstring>

namespace wiiuport::guest {

void LineProbe::install() {
    GuestCallProbes::Register(kFlushSet, kFlushSetFirstInstruction, *this);
}

void LineProbe::OnInstall(GuestCallProbes::Installation installation) {
    if (installation == GuestCallProbes::Installation::Installed) {
        m_writers.noteInstalled();
    }
}

const void* LineProbe::listOf(uint32_t set, uint32_t list) {
    return GuestCallProbes::GuestBytes(set + (list * kListBytes), kListBytes);
}

uint32_t LineProbe::bufferOf(const void* list, uint32_t index) {
    // The helper flushes the list's first buffer for any index past its count.
    uint32_t first = guestWord(list, 4);
    return index < guestWord(list, 0) ? first + (index * kBufferStride) : first;
}

void LineProbe::OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) {
    if (returnAddress != kLineUpdateReturn && returnAddress != kOtherLineUpdateReturn) {
        return;
    }
    uint32_t set = gpr[kSetRegister];
    const void* setBytes = GuestCallProbes::GuestBytes(set, kFlip + 4);
    uint32_t flip = setBytes == nullptr ? 0 : guestWord(setBytes, kFlip);
    // The list just written, and the first, whose buffers name the line's.
    const void* written = setBytes == nullptr || flip > 1 ? nullptr : listOf(set, flip);
    const void* naming = listOf(set, 0);
    for (uint32_t index = 0; index < gpr[kCountRegister]; ++index) {
        const void* buffer =
            written == nullptr
                ? nullptr
                : GuestCallProbes::GuestBytes(bufferOf(written, index), kBufferVertices + 4);
        const void* source = buffer == nullptr
                                 ? nullptr
                                 : GuestCallProbes::GuestBytes(guestWord(buffer, kBufferVertices),
                                                               BufferWriters::kLeadingBytes);
        if (source == nullptr || naming == nullptr) {
            m_writers.noteUnreadable();
            continue;
        }
        BufferWriters::Leading leading{};
        std::memcpy(leading.data(), source, leading.size());
        m_writers.record(source, {.address = bufferOf(naming, index), .age = std::nullopt},
                         leading);
    }
}

} // namespace wiiuport::guest
