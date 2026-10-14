#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace wiiuport::shell {

// The running title's save files for the current account, read as the title's save library finds
// them.
class TitleSave {
  public:
    // The file's bytes, or none when the title has not written it.
    static std::optional<std::vector<uint8_t>> read(std::string_view file);
};

} // namespace wiiuport::shell
