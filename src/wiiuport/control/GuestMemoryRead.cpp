#include "wiiuport/control/GuestMemoryRead.h"

#include <charconv>
#include <system_error>

namespace wiiuport::control {

namespace {

std::optional<uint32_t> number(std::string_view text, int base) {
    uint32_t value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::optional<GuestMemoryRead::Request> GuestMemoryRead::parse(std::string_view query,
                                                               std::string& refusal) {
    std::optional<uint32_t> start;
    std::optional<uint32_t> count;
    while (!query.empty()) {
        size_t end = query.find('&');
        std::string_view pair = query.substr(0, end);
        query = end == std::string_view::npos ? std::string_view{} : query.substr(end + 1);
        size_t equals = pair.find('=');
        std::string_view key = pair.substr(0, equals);
        std::string_view value =
            equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
        if (key == "address") {
            start = number(value, 16);
        } else if (key == "size") {
            count = number(value, 10);
        }
    }
    if (!start.has_value() || !count.has_value()) {
        refusal = "name address=<hex> and size=<decimal>.\n";
        return std::nullopt;
    }
    if (*count == 0 || *count > kMaxBytes) {
        refusal = "size must be 1 to 16777216 bytes.\n";
        return std::nullopt;
    }
    if (*start > UINT32_MAX - *count) {
        refusal = "address + size passes the end of the guest's address space.\n";
        return std::nullopt;
    }
    return Request{.address = *start, .size = *count};
}

std::optional<std::string> GuestMemoryRead::read(Request request, GuestBytes guestBytes) {
    const void* bytes = guestBytes(request.address, request.size);
    if (bytes == nullptr) {
        return std::nullopt;
    }
    return std::string(static_cast<const char*>(bytes), request.size);
}

} // namespace wiiuport::control
