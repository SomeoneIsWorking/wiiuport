#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wiiuport::shell {

// The one title a product runs, as its launcher names it.
//
// The runtime knows no title of its own: a consuming product passes the ID of
// the title it was made for, and a disc image holding any other is refused
// before it is remembered or launched, with what it holds named. Without an
// expected title every mountable one is accepted, as a maintainer's runtime
// does.
class TitleIdentity {
  public:
    // Sixteen hexadecimal digits, as Wii U title IDs are written
    // (0005000010143500). Anything else is not an ID.
    static std::optional<TitleIdentity> parse(std::string_view text);

    explicit TitleIdentity(uint64_t id) : m_id(id) {
    }

    uint64_t id() const {
        return m_id;
    }

    // Empty when a disc image holding `found`, named `foundName` by its own
    // metadata, is this title; otherwise the reason a player is shown.
    std::string refusal(uint64_t found, std::string_view foundName) const;

    // The ID as it is written: sixteen hexadecimal digits.
    static std::string format(uint64_t id);

  private:
    uint64_t m_id;
};

} // namespace wiiuport::shell
