#include "wiiuport/shell/ProductCommandLine.h"

#include <charconv>
#include <utility>

namespace wiiuport::shell {
namespace {

// A window dimension in pixels: a whole, positive number and nothing else.
std::optional<int> readDimension(std::string_view text) {
    int value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value <= 0) {
        return std::nullopt;
    }
    return value;
}

bool takesValue(std::string_view argument) {
    return argument == "--title-id" || argument == "--product-name" || argument == "--width" ||
           argument == "--height";
}

ParsedCommandLine refused(std::string error) {
    return ParsedCommandLine{.request = {}, .error = std::move(error)};
}

} // namespace

ParsedCommandLine parseCommandLine(const Product& product,
                                   std::span<const std::string_view> arguments) {
    LaunchRequest request;
    request.productName = product.name;
    request.expectedTitle = product.title;
    for (size_t index = 0; index < arguments.size(); ++index) {
        std::string_view argument = arguments[index];
        if (takesValue(argument) && index + 1 == arguments.size()) {
            return refused(std::string(argument) + " needs a value");
        }
        if (argument == "--hidden") {
            request.hidden = true;
        } else if (argument == "--title-id") {
            std::string_view text = arguments[++index];
            std::optional<TitleIdentity> named = TitleIdentity::parse(text);
            if (!named.has_value()) {
                return refused("--title-id takes sixteen hex digits, not " + std::string(text));
            }
            // Fixed by the product or by an earlier --title-id: a launcher's
            // own argument cannot be replaced by one appended after it.
            if (request.expectedTitle.has_value() && request.expectedTitle->id() != named->id()) {
                return refused(request.displayName() + " runs " +
                               TitleIdentity::format(request.expectedTitle->id()) + ", not " +
                               TitleIdentity::format(named->id()));
            }
            request.expectedTitle = named;
        } else if (argument == "--product-name") {
            std::string named(arguments[++index]);
            if (named.empty()) {
                return refused("--product-name takes a name, not nothing");
            }
            if (request.productName.has_value() && *request.productName != named) {
                return refused("this product is " + *request.productName + ", not " + named);
            }
            request.productName = std::move(named);
        } else if (argument == "--width" || argument == "--height") {
            std::string_view text = arguments[++index];
            std::optional<int> pixels = readDimension(text);
            if (!pixels.has_value()) {
                return refused(std::string(argument) + " takes a positive whole number, not " +
                               std::string(text));
            }
            (argument == "--width" ? request.width : request.height) = pixels;
        } else if (!argument.empty() && argument.front() == '-') {
            return refused("unknown option " + std::string(argument));
        } else if (!request.title.empty()) {
            return refused("more than one disc image given; a product runs one title");
        } else {
            request.title = std::filesystem::path(argument);
        }
    }
    return ParsedCommandLine{.request = std::move(request), .error = {}};
}

} // namespace wiiuport::shell
