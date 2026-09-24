#include "check.h"
#include "suites.h"
#include "wiiuport/shell/ProductCommandLine.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using wiiuport::shell::parseCommandLine;
using wiiuport::shell::ParsedCommandLine;
using wiiuport::shell::Product;
using wiiuport::shell::TitleIdentity;

namespace {

constexpr uint64_t kWindWaker = 0x0005000010143500;

ParsedCommandLine parsed(const Product& product, std::vector<std::string_view> arguments) {
    return parseCommandLine(product, arguments);
}

Product fixedTo(uint64_t title) {
    return Product{.name = "A product", .title = TitleIdentity(title)};
}

void noArgumentsIsAPlayersLaunch() {
    ParsedCommandLine line = parsed({}, {});
    check::isTrue(line.error.empty(), "a product started with nothing is a launch");
    check::isTrue(line.request.title.empty(), "that asks for the remembered title");
    check::isTrue(!line.request.expectedTitle.has_value(), "and the runtime accepts any title");
    check::isTrue(!line.request.hidden, "in a window the player can see");
}

void aProductsTitleIsExpectedWithoutBeingAsked() {
    ParsedCommandLine line = parsed(fixedTo(kWindWaker), {});
    check::isTrue(line.request.expectedTitle.has_value() &&
                      line.request.expectedTitle->id() == kWindWaker,
                  "the title a product fixes is the one it expects");
}

void aMaintainerNamesTheTitleOnTheGenericRuntime() {
    ParsedCommandLine line = parsed({}, {"disc.wud", "--title-id", "0005000010143500", "--hidden",
                                         "--width", "640", "--height", "360"});
    check::isTrue(line.error.empty(), "every option is read");
    check::isTrue(line.request.title == "disc.wud", "the disc image is the one given");
    check::isTrue(line.request.expectedTitle.has_value() &&
                      line.request.expectedTitle->id() == kWindWaker,
                  "the named title is expected");
    check::isTrue(line.request.hidden, "the window is hidden");
    check::isTrue(line.request.width == 640 && line.request.height == 360,
                  "at the size asked for, width and height each in its place");
}

void aProductRefusesToBeToldToRunAnotherTitle() {
    ParsedCommandLine same = parsed(fixedTo(kWindWaker), {"--title-id", "0005000010143500"});
    check::isTrue(same.error.empty(), "naming its own title is accepted");
    ParsedCommandLine other = parsed(fixedTo(kWindWaker), {"--title-id", "00050000101c9400"});
    check::isTrue(other.error.find("00050000101c9400") != std::string::npos,
                  "naming another is refused, and says which");
    ParsedCommandLine appended =
        parsed({}, {"--title-id", "0005000010143500", "--title-id", "00050000101c9400"});
    check::isTrue(!appended.error.empty(),
                  "a launcher's title is not replaced by one appended after it");
}

void theProductsNameIsWhatThePlayerSees() {
    check::isTrue(parsed({}, {}).request.displayName() == "wiiuport",
                  "a product that names nothing is the runtime");
    check::isTrue(parsed(fixedTo(kWindWaker), {}).request.displayName() == "A product",
                  "a product is called what it names itself");
    ParsedCommandLine named = parsed({}, {"--product-name", "Set Sail"});
    check::isTrue(named.error.empty() && named.request.displayName() == "Set Sail",
                  "a launcher names the generic runtime's product");
    ParsedCommandLine renamed =
        parsed({}, {"--product-name", "Set Sail", "--product-name", "Another"});
    check::isTrue(renamed.error.find("Another") != std::string::npos,
                  "a launcher's name is not replaced by one appended after it");
    check::isTrue(parsed(fixedTo(kWindWaker), {"--product-name", "A product"}).error.empty(),
                  "a product may be given its own name");
    check::isTrue(parsed(fixedTo(kWindWaker), {"--title-id", "00050000101c9400"})
                          .error.find("A product runs") != std::string::npos,
                  "a refusal names the product");
}

void aMalformedCommandLineIsRefusedNotGuessed() {
    for (const std::vector<std::string_view>& arguments :
         std::vector<std::vector<std::string_view>>{{"--width"},
                                                    {"--width", "wide"},
                                                    {"--height", "-5"},
                                                    {"--height", "360px"},
                                                    {"--title-id", "10143500"},
                                                    {"--product-name"},
                                                    {"--product-name", ""},
                                                    {"--fullscreen"},
                                                    {"one.wud", "two.wud"}}) {
        std::string shown;
        for (std::string_view argument : arguments) {
            shown += std::string(argument) + " ";
        }
        check::isTrue(!parsed({}, arguments).error.empty(), "refused: " + shown);
    }
}

} // namespace

namespace wiiuport::tests {

void runProductTests() {
    noArgumentsIsAPlayersLaunch();
    aProductsTitleIsExpectedWithoutBeingAsked();
    aMaintainerNamesTheTitleOnTheGenericRuntime();
    aProductRefusesToBeToldToRunAnotherTitle();
    theProductsNameIsWhatThePlayerSees();
    aMalformedCommandLineIsRefusedNotGuessed();
}

} // namespace wiiuport::tests
