#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wiiuport::control {

// GET /memory?address=<hex>&size=<decimal>: the guest's bytes as they are,
// so a maintainer can find where the title keeps a value it is known to
// hold. Bounded, and refused by reason rather than answered short.
class GuestMemoryRead {
  public:
    static constexpr uint32_t kMaxBytes = 16u << 20;

    struct Request {
        uint32_t address = 0;
        uint32_t size = 0;
    };

    // The request the query names, or why it names none.
    static std::optional<Request> parse(std::string_view query, std::string& refusal);

    // The host bytes behind a guest range, or null unless all of it is
    // mapped: the fork's GuestCallProbes::GuestBytes, injected.
    using GuestBytes = const void* (*)(uint32_t address, uint32_t size);

    // The bytes, or std::nullopt when any of them is not guest memory.
    static std::optional<std::string> read(Request request, GuestBytes guestBytes);
};

} // namespace wiiuport::control
