#include "wiiuport/shell/TitleIdentity.h"

#include <charconv>
#include <format>

namespace wiiuport::shell {

std::optional<TitleIdentity> TitleIdentity::parse(std::string_view text) {
    if (text.size() != 16) {
        return std::nullopt;
    }
    uint64_t id = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id, 16);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
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
