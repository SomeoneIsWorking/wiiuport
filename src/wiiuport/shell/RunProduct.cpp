#include "wiiuport/shell/RunProduct.h"

#include "wiiuport/Runtime.h"
#include "wiiuport/shell/ShellHost.h"

#include <lucent/log.h>

#include <string_view>
#include <vector>

namespace wiiuport::shell {

int runProduct(const Product& product, int argc, char** argv) {
    std::vector<std::string_view> arguments(argv + 1, argv + argc);
    ParsedCommandLine parsed = parseCommandLine(product, arguments);
    if (!parsed.error.empty()) {
        lucent::error("shell", "{}", parsed.error);
        lucent::error("shell",
                      "usage: {} [disc image] [--title-id ID] [--product-name NAME] [--hidden] "
                      "[--width N] [--height N]",
                      argv[0]);
        return 2;
    }
    ShellHost::Options options;
    options.executable = argv[0];
    options.title = parsed.request.title;
    options.expectedTitle = parsed.request.expectedTitle;
    options.window.title = parsed.request.displayName();
    options.window.width = parsed.request.width.value_or(options.window.width);
    options.window.height = parsed.request.height.value_or(options.window.height);
    options.window.hidden = parsed.request.hidden;

    // Installed before the system exists, so the first frame the guest
    // submits is already observed.
    Runtime::instance().installHooks();
    ShellHost host;
    return host.run(options);
}

} // namespace wiiuport::shell
