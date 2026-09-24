#pragma once

#include "wiiuport/shell/TitleIdentity.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wiiuport::shell {

// The name the runtime goes by when no product names itself.
inline constexpr std::string_view kRuntimeName = "wiiuport";

// What a title project fixes about the product it builds on this runtime.
//
// It is the whole of a title's say over the host: its name, shown on its
// window and setup screen, and the one title it runs. A product that fixes
// neither is the maintainer's runtime, which runs any mountable title.
struct Product {
    std::optional<std::string> name;
    std::optional<TitleIdentity> title;
};

// One launch as the command line asks for it. Every field is optional to a
// player: a packaged product is started with no arguments at all.
struct LaunchRequest {
    // Empty asks for the remembered title, and then the player.
    std::filesystem::path title;
    std::optional<std::string> productName;
    std::optional<TitleIdentity> expectedTitle;
    std::optional<int> width;
    std::optional<int> height;
    bool hidden{false};

    // What the player sees the product called.
    std::string displayName() const {
        return productName.value_or(std::string(kRuntimeName));
    }
};

// A launch request, or why the command line does not make one.
struct ParsedCommandLine {
    LaunchRequest request;
    std::string error;
};

// Reads `arguments` (without the program name) for `product`.
//
// `--title-id` and `--product-name` are how a launcher that is not C++ fixes
// what a product would on the generic runtime. Once either is fixed -- by the
// product or an earlier argument -- another naming something different is
// refused, never settled silently either way.
ParsedCommandLine parseCommandLine(const Product& product,
                                   std::span<const std::string_view> arguments);

} // namespace wiiuport::shell
