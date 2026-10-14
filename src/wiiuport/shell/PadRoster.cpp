#include "wiiuport/shell/PadRoster.h"

#include <algorithm>
#include <utility>

namespace wiiuport::shell {

PadRoster::PadRoster(std::vector<std::string> mapped) : m_mapped(std::move(mapped)) {
}

PadRoster::Change PadRoster::against(std::span<const std::string> present) const {
    Change change;
    for (const std::string& id : m_mapped) {
        if (std::find(present.begin(), present.end(), id) != present.end()) {
            change.keep = true;
        } else {
            change.lost.push_back(id);
        }
    }
    return change;
}

} // namespace wiiuport::shell
