#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wiiuport::title {

// Where the binding core's next GX2 packet goes, as host addresses into the guest's command buffer.
struct CommandPosition {
    uintptr_t bufferStart = 0;
    uintptr_t bufferEnd = 0;
    uintptr_t write = 0;
};

// Which object a draw belongs to, joined through the command stream.
//
// The binder runs on the guest thread that writes the command buffer; the draw is executed later
// on the Latte thread. So each bind is recorded at its write position, and a draw is named by the
// last bind written before its packet in the same buffer.
class CommandStreamIdentity {
  public:
    using ReadPosition = std::function<CommandPosition()>;

    explicit CommandStreamIdentity(ReadPosition readPosition)
        : m_readPosition(std::move(readPosition)) {
    }

    // Guest thread: the object the binder named, at the current write position.
    void bind(uint32_t object);

    // Latte thread: the object bound before this packet, or zero.
    uint32_t objectAt(uintptr_t packet) const;

    struct Report {
        uint64_t binds = 0;
        uint64_t bindsWithoutBuffer = 0;
        uint64_t buffersRestarted = 0;
        uint64_t buffersDropped = 0;
        uint64_t buffersTracked = 0;
        uint64_t lookups = 0;
        uint64_t lookupsNamed = 0;
        uint64_t lookupsWithoutPacket = 0;
        uint64_t lookupsWithoutBuffer = 0;
        uint64_t lookupsBeforeFirstBind = 0;
    };

    Report report() const;

    std::string json() const;

    // Buffers kept at once; the oldest written is dropped beyond this.
    static constexpr size_t kBuffers = 64;

  private:
    struct Buffer {
        uintptr_t end = 0;
        uint64_t lastWritten = 0;
        std::vector<std::pair<uintptr_t, uint32_t>> binds;
    };

    void dropOverlapping(uintptr_t start, uintptr_t end);

    ReadPosition m_readPosition;
    mutable std::mutex m_mutex;
    std::map<uintptr_t, Buffer> m_buffers;
    uint64_t m_writes = 0;
    mutable Report m_report;
};

} // namespace wiiuport::title
