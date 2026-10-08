#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <optional>
#include <string_view>

namespace wiiuport::guest {

// How a probe's registration ended, as reports name it; "pending" before the title is linked.
inline std::string_view installationName(std::optional<GuestCallProbes::Installation> value) {
    if (!value.has_value()) {
        return "pending";
    }
    switch (*value) {
    case GuestCallProbes::Installation::Installed:
        return "installed";
    case GuestCallProbes::Installation::EntryHeldOther:
        return "entryHeldOther";
    case GuestCallProbes::Installation::EntryNotRelocatable:
        return "entryNotRelocatable";
    case GuestCallProbes::Installation::NoCodeSpace:
        return "noCodeSpace";
    }
    return "unknown";
}

} // namespace wiiuport::guest
