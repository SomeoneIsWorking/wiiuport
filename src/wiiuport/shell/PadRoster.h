#pragma once

#include <span>
#include <string>
#include <vector>

namespace wiiuport::shell {

// Compares the pads mapped to a player with the pads plugged in, by device id.
class PadRoster {
  public:
    struct Change {
        // Mapped but no longer plugged in: to be unmapped and counted once.
        std::vector<std::string> lost;
        // A mapped pad is still plugged in, so the mapping stands.
        bool keep = false;
    };

    explicit PadRoster(std::vector<std::string> mapped);

    Change against(std::span<const std::string> present) const;

  private:
    std::vector<std::string> m_mapped;
};

} // namespace wiiuport::shell
