#include "wiiuport/shell/TitleIdentity.h"

#include <format>

namespace wiiuport::shell {

std::optional<TitleIdentity> TitleIdentity::parse(std::string_view text) {
    if (text.size() != 16) {
        return std::nullopt;
    }
    uint64_t id = 0;
    for (char digit : text) {
        uint64_t value = 0;
        if (digit >= '0' && digit <= '9') {
            value = static_cast<uint64_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            value = static_cast<uint64_t>(digit - 'a' + 10);
        } else if (digit >= 'A' && digit <= 'F') {
            value = static_cast<uint64_t>(digit - 'A' + 10);
        } else {
            return std::nullopt;
        }
        id = (id << 4) | value;
    }
    return TitleIdentity(id);
}

std::string TitleIdentity::format(uint64_t id) {
    return std::format("{:016x}", id);
}

std::string TitleIdentity::refusal(uint64_t found, std::string_view foundName) const {
    if (found == m_id) {
        return {};
    }
    std::string named = foundName.empty() ? std::string("a title") : std::string(foundName);
    return "this disc holds " + named + " (" + format(found) +
           "), not the game this product runs (" + format(m_id) + ")";
}

} // namespace wiiuport::shell
